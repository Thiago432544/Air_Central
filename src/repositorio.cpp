#include "repositorio.h"

#include <algorithm>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {

const QStringList DIAS = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"};

// Valor NULL do SQL para campos opcionais (id -1 / temp -1).
QVariant ouNulo(int valor)
{
    return valor < 0 ? QVariant(QMetaType(QMetaType::Int)) : QVariant(valor);
}

bool rodar(QSqlQuery &q)
{
    if (!q.exec()) {
        qWarning() << "[Repositorio] Erro SQL:" << q.lastError().text() << "|" << q.lastQuery();
        return false;
    }
    return true;
}

} // namespace

Repositorio::Repositorio(QObject *parent)
    : QObject(parent), nomeConexao("air_central")
{
}

Repositorio::~Repositorio()
{
    if (db.isOpen()) db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(nomeConexao);
}

// =======================================================================
// Abertura, schema e migração
// =======================================================================
bool Repositorio::abrir(const QString &caminhoArquivo)
{
    caminho = caminhoArquivo;
    if (caminho.isEmpty()) {
        QString pasta = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(pasta);
        caminho = pasta + "/air_central.db";
    }

    db = QSqlDatabase::addDatabase("QSQLITE", nomeConexao);
    db.setDatabaseName(caminho);

    if (!db.open()) {
        qCritical() << "[Repositorio] Não foi possível abrir o banco:" << caminho << db.lastError().text();
        return false;
    }

    // Sem isso o SQLite ignora as chaves estrangeiras (CASCADE / SET NULL).
    executar("PRAGMA foreign_keys = ON");

    QSqlQuery q(db);
    q.exec("PRAGMA user_version");
    int versao = q.next() ? q.value(0).toInt() : 0;

    if (versao == 0) {
        if (!criarSchema()) return false;
        migrarJsonLegado();
    }

    qDebug() << "[Repositorio] Banco aberto em" << caminho;
    return true;
}

bool Repositorio::criarSchema()
{
    QFile arquivo(":/src/schema.sql");
    if (!arquivo.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "[Repositorio] schema.sql não encontrado nos recursos";
        return false;
    }

    QString sql = QString::fromUtf8(arquivo.readAll());
    sql.remove(QRegularExpression("--[^\n]*"));

    db.transaction();
    const QStringList comandos = sql.split(';');
    for (const QString &comando : comandos) {
        if (comando.trimmed().isEmpty()) continue;
        if (!executar(comando)) {
            db.rollback();
            return false;
        }
    }
    executar("PRAGMA user_version = 1");
    db.commit();

    qDebug() << "[Repositorio] Banco criado (schema v1)";
    return true;
}

// Importa codes.json e agendamentos.json da versão antiga, uma única vez.
// Depois de importados, os arquivos são renomeados para *.migrado.json.
void Repositorio::migrarJsonLegado()
{
    const QStringList pastas = {QDir::currentPath(), QCoreApplication::applicationDirPath()};

    auto procurar = [&pastas](const QString &nome) -> QString {
        for (const QString &pasta : pastas) {
            QString c = pasta + "/" + nome;
            if (QFile::exists(c)) return c;
        }
        return QString();
    };

    auto lerJson = [](const QString &arquivo) -> QJsonObject {
        QFile f(arquivo);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    };

    auto arquivar = [](const QString &arquivo) {
        QString destino = arquivo;
        destino.replace(".json", ".migrado.json");
        QFile::remove(destino);
        QFile::rename(arquivo, destino);
    };

    db.transaction();

    // --- codes.json: { "Ligar": [raw_modelo1, raw_modelo2, ...], "22": [...] }
    QString arquivoCodigos = procurar("codes.json");
    int totalCodigos = 0;
    if (!arquivoCodigos.isEmpty()) {
        QJsonObject codigos = lerJson(arquivoCodigos);
        for (auto it = codigos.begin(); it != codigos.end(); ++it) {
            QString comando = comandoCanonico(it.key());
            if (comando.isEmpty()) continue;

            const QJsonArray lista = it.value().toArray();
            for (int i = 0; i < lista.size(); ++i) {
                QString dados = lista[i].toString();
                if (dados.isEmpty()) continue;

                // A posição na lista era o modelo (ordem de captura).
                int modeloId = modeloPorNome(QString("Modelo %1").arg(i + 1), true);
                if (modeloId >= 0 && salvarCodigo(modeloId, comando, dados)) totalCodigos++;
            }
        }
    }

    // --- agendamentos.json: { "Sala": [ {dia, hora, acao, temp}, ... ] }
    QString arquivoAgenda = procurar("agendamentos.json");
    int totalRegras = 0;
    if (!arquivoAgenda.isEmpty()) {
        QJsonObject agenda = lerJson(arquivoAgenda);
        for (auto it = agenda.begin(); it != agenda.end(); ++it) {
            int salaId = salaPorNome(it.key(), true);
            if (salaId < 0) continue;

            const QJsonArray regras = it.value().toArray();
            for (const QJsonValue &valor : regras) {
                QJsonObject r = valor.toObject();

                Agendamento a;
                a.salaId = salaId;
                a.dia    = diaParaNumero(r["dia"].toString());
                a.hora   = r["hora"].toString();
                a.acao   = comandoCanonico(r["acao"].toString());
                bool ok  = false;
                int temp = r["temp"].toString().toInt(&ok);
                a.temp   = ok ? temp : -1;
                a.origem = "legado";

                if (a.dia < 0 || (a.acao != "LIGAR" && a.acao != "DESLIGAR")) continue;
                if (adicionarAgendamento(a) >= 0) totalRegras++;
            }
        }
    }

    db.commit();

    if (!arquivoCodigos.isEmpty()) arquivar(arquivoCodigos);
    if (!arquivoAgenda.isEmpty())  arquivar(arquivoAgenda);

    if (totalCodigos || totalRegras) {
        qDebug() << "[Repositorio] Migração dos JSON antigos:" << totalCodigos << "códigos e"
                 << totalRegras << "agendamentos importados";
    }
}

bool Repositorio::executar(const QString &sql)
{
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        qWarning() << "[Repositorio] Erro SQL:" << q.lastError().text() << "|" << sql.simplified();
        return false;
    }
    return true;
}

// =======================================================================
// Modelos
// =======================================================================
QList<Modelo> Repositorio::modelos() const
{
    QList<Modelo> lista;
    QSqlQuery q(db);
    q.prepare("SELECT id, nome, marca, formato FROM modelos ORDER BY id");
    if (!rodar(q)) return lista;

    while (q.next()) {
        Modelo m;
        m.id      = q.value(0).toInt();
        m.nome    = q.value(1).toString();
        m.marca   = q.value(2).toString();
        m.formato = q.value(3).toString();
        lista.append(m);
    }
    return lista;
}

int Repositorio::criarModelo(const QString &nome, const QString &marca)
{
    // Checa antes para não depender do erro do UNIQUE (que sujaria o log).
    if (nome.trimmed().isEmpty() || modeloPorNome(nome, false) >= 0) return -1;

    QSqlQuery q(db);
    q.prepare("INSERT INTO modelos (nome, marca) VALUES (?, ?)");
    q.addBindValue(nome.trimmed());
    q.addBindValue(marca.trimmed());
    if (!rodar(q)) return -1;

    int id = q.lastInsertId().toInt();
    emit modelosAlterados();
    return id;
}

int Repositorio::modeloPorNome(const QString &nome, bool criar)
{
    QSqlQuery q(db);
    q.prepare("SELECT id FROM modelos WHERE nome = ?");
    q.addBindValue(nome.trimmed());
    if (rodar(q) && q.next()) return q.value(0).toInt();
    return criar ? criarModelo(nome) : -1;
}

bool Repositorio::renomearModelo(int modeloId, const QString &nome, const QString &marca)
{
    if (nome.trimmed().isEmpty()) return false;
    int mesmoNome = modeloPorNome(nome, false);
    if (mesmoNome >= 0 && mesmoNome != modeloId) return false;   // nome de outro modelo

    QSqlQuery q(db);
    q.prepare("UPDATE modelos SET nome = ?, marca = ? WHERE id = ?");
    q.addBindValue(nome.trimmed());
    q.addBindValue(marca.trimmed());
    q.addBindValue(modeloId);
    if (!rodar(q) || q.numRowsAffected() == 0) return false;

    emit modelosAlterados();
    return true;
}

bool Repositorio::apagarModelo(int modeloId)
{
    // Os códigos do modelo saem junto (CASCADE); as salas ficam sem modelo.
    QSqlQuery q(db);
    q.prepare("DELETE FROM modelos WHERE id = ?");
    q.addBindValue(modeloId);
    if (!rodar(q) || q.numRowsAffected() == 0) return false;

    emit modelosAlterados();
    return true;
}

QStringList Repositorio::comandosValidos()
{
    QStringList lista = {"LIGAR", "DESLIGAR"};
    for (int t = 16; t <= 30; ++t) lista << QString("T%1").arg(t);
    return lista;
}

QVariantList Repositorio::listarModelos() const
{
    QVariantList lista;
    QSqlQuery q(db);
    q.prepare("SELECT m.id, m.nome, m.marca, m.formato, "
              "  (SELECT COUNT(*) FROM codigos c WHERE c.modelo_id = m.id), "
              "  (SELECT COUNT(*) FROM salas s WHERE s.modelo_id = m.id) "
              "FROM modelos m ORDER BY m.id");
    if (!rodar(q)) return lista;

    while (q.next()) {
        QVariantMap m;
        m["id"]      = q.value(0).toInt();
        m["nome"]    = q.value(1).toString();
        m["marca"]   = q.value(2).toString();
        m["formato"] = q.value(3).toString();
        m["codigos"] = q.value(4).toInt();
        m["salas"]   = q.value(5).toInt();
        lista.append(m);
    }
    return lista;
}

QVariantMap Repositorio::comandosDoModelo(int modeloId) const
{
    const QMap<QString, QString> codigos = codigosDoModelo(modeloId);
    QVariantMap mapa;
    for (const QString &comando : comandosValidos()) {
        mapa[comando] = codigos.contains(comando);
    }
    return mapa;
}

// =======================================================================
// Códigos IR
// =======================================================================
bool Repositorio::salvarCodigo(int modeloId, const QString &comando, const QString &dados)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO codigos (modelo_id, comando, dados) VALUES (?, ?, ?) "
              "ON CONFLICT (modelo_id, comando) DO UPDATE SET "
              "dados = excluded.dados, capturado_em = datetime('now')");
    q.addBindValue(modeloId);
    q.addBindValue(comando);
    q.addBindValue(dados);
    if (!rodar(q)) return false;

    emit modelosAlterados();
    return true;
}

bool Repositorio::apagarCodigo(int modeloId, const QString &comando)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM codigos WHERE modelo_id = ? AND comando = ?");
    q.addBindValue(modeloId);
    q.addBindValue(comando);
    if (!rodar(q) || q.numRowsAffected() == 0) return false;

    emit modelosAlterados();
    return true;
}

QMap<QString, QString> Repositorio::codigosDoModelo(int modeloId) const
{
    QMap<QString, QString> mapa;
    QSqlQuery q(db);
    q.prepare("SELECT comando, dados FROM codigos WHERE modelo_id = ?");
    q.addBindValue(modeloId);
    if (!rodar(q)) return mapa;

    while (q.next()) mapa.insert(q.value(0).toString(), q.value(1).toString());
    return mapa;
}

bool Repositorio::codigoJaExiste(const QString &comando, const QString &dados) const
{
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM codigos WHERE comando = ? AND dados = ? LIMIT 1");
    q.addBindValue(comando);
    q.addBindValue(dados);
    return rodar(q) && q.next();
}

int Repositorio::adicionarCodigoProximoModelo(const QString &comando, const QString &dados)
{
    QSqlQuery q(db);
    q.prepare("SELECT id FROM modelos m WHERE NOT EXISTS ("
              "  SELECT 1 FROM codigos c WHERE c.modelo_id = m.id AND c.comando = ?) "
              "ORDER BY id LIMIT 1");
    q.addBindValue(comando);

    int modeloId = -1;
    if (rodar(q) && q.next()) {
        modeloId = q.value(0).toInt();
    } else {
        // Todos os modelos já têm esse comando: cria o próximo "Modelo N".
        int n = modelos().size() + 1;
        while (modeloPorNome(QString("Modelo %1").arg(n), false) >= 0) n++;
        modeloId = criarModelo(QString("Modelo %1").arg(n));
    }

    if (modeloId < 0 || !salvarCodigo(modeloId, comando, dados)) return -1;
    return modeloId;
}

bool Repositorio::apagarUltimoCodigo(const QString &comando)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM codigos WHERE comando = ? AND modelo_id = ("
              "  SELECT MAX(modelo_id) FROM codigos WHERE comando = ?)");
    q.addBindValue(comando);
    q.addBindValue(comando);
    if (!rodar(q) || q.numRowsAffected() == 0) return false;

    emit modelosAlterados();
    return true;
}

bool Repositorio::apagarTodosCodigos()
{
    // Os modelos continuam (podem estar ligados a salas); só os códigos saem.
    if (!executar("DELETE FROM codigos")) return false;

    emit modelosAlterados();
    return true;
}

int Repositorio::contarCodigos(const QString &comando) const
{
    QSqlQuery q(db);
    q.prepare("SELECT COUNT(*) FROM codigos WHERE comando = ?");
    q.addBindValue(comando);
    return (rodar(q) && q.next()) ? q.value(0).toInt() : 0;
}

QMap<QString, QString> Repositorio::codigosDoUltimoModelo() const
{
    QMap<QString, QString> mapa;
    QSqlQuery q(db);
    q.prepare("SELECT comando, dados FROM codigos c WHERE modelo_id = ("
              "  SELECT MAX(modelo_id) FROM codigos c2 WHERE c2.comando = c.comando)");
    if (!rodar(q)) return mapa;

    while (q.next()) mapa.insert(q.value(0).toString(), q.value(1).toString());
    return mapa;
}

// =======================================================================
// Salas
// =======================================================================
QList<Sala> Repositorio::salas() const
{
    QList<Sala> lista;
    QSqlQuery q(db);
    q.prepare("SELECT id, nome, bloco, modelo_id, temp_padrao FROM salas ORDER BY nome");
    if (!rodar(q)) return lista;

    while (q.next()) {
        Sala s;
        s.id         = q.value(0).toInt();
        s.nome       = q.value(1).toString();
        s.bloco      = q.value(2).toString();
        s.modeloId   = q.value(3).isNull() ? -1 : q.value(3).toInt();
        s.tempPadrao = q.value(4).toInt();
        lista.append(s);
    }
    return lista;
}

Sala Repositorio::sala(int salaId) const
{
    QSqlQuery q(db);
    q.prepare("SELECT id, nome, bloco, modelo_id, temp_padrao FROM salas WHERE id = ?");
    q.addBindValue(salaId);

    Sala s;
    if (rodar(q) && q.next()) {
        s.id         = q.value(0).toInt();
        s.nome       = q.value(1).toString();
        s.bloco      = q.value(2).toString();
        s.modeloId   = q.value(3).isNull() ? -1 : q.value(3).toInt();
        s.tempPadrao = q.value(4).toInt();
    }
    return s;
}

int Repositorio::salaPorNome(const QString &nome, bool criar)
{
    QString limpo = nome.trimmed();
    if (limpo.isEmpty()) return -1;

    QSqlQuery q(db);
    q.prepare("SELECT id FROM salas WHERE nome = ?");
    q.addBindValue(limpo);
    if (rodar(q) && q.next()) return q.value(0).toInt();
    if (!criar) return -1;

    QSqlQuery ins(db);
    ins.prepare("INSERT INTO salas (nome) VALUES (?)");
    ins.addBindValue(limpo);
    if (!rodar(ins)) return -1;
    return ins.lastInsertId().toInt();
}

bool Repositorio::definirModeloSala(int salaId, int modeloId)
{
    QSqlQuery q(db);
    q.prepare("UPDATE salas SET modelo_id = ? WHERE id = ?");
    q.addBindValue(ouNulo(modeloId));
    q.addBindValue(salaId);
    if (!rodar(q) || q.numRowsAffected() == 0) return false;

    emit modelosAlterados();
    return true;
}

QVariantMap Repositorio::modeloDoDispositivo(const QString &idDispositivo)
{
    QVariantMap resultado;
    resultado["id"] = -1;
    resultado["nome"] = QString();

    // Não cria sala só para consultar: placa sem sala = sem modelo.
    int salaId = salaDoDispositivo(idDispositivo, false);
    if (salaId < 0) return resultado;

    int modeloId = sala(salaId).modeloId;
    if (modeloId < 0) return resultado;

    resultado["id"] = modeloId;
    resultado["nome"] = modeloNome(modeloId);
    return resultado;
}

bool Repositorio::definirModeloDoDispositivo(const QString &idDispositivo, int modeloId)
{
    int salaId = salaDoDispositivo(idDispositivo, true);
    if (salaId < 0) return false;
    return definirModeloSala(salaId, modeloId);
}

int Repositorio::salaDoDispositivo(const QString &idDispositivo, bool criar)
{
    QSqlQuery q(db);
    q.prepare("SELECT sala_id FROM nos WHERE chip_id = ? AND sala_id IS NOT NULL");
    q.addBindValue(idDispositivo);
    if (rodar(q) && q.next()) return q.value(0).toInt();

    int salaId = salaPorNome(idDispositivo, criar);
    if (salaId >= 0) vincularPorNome(idDispositivo);
    return salaId;
}

QString Repositorio::placaDaSala(int salaId) const
{
    QSqlQuery q(db);
    q.prepare("SELECT chip_id FROM nos WHERE sala_id = ?");
    q.addBindValue(salaId);
    return (rodar(q) && q.next()) ? q.value(0).toString() : QString();
}

QString Repositorio::destinoDaSala(int salaId) const
{
    QString placa = placaDaSala(salaId);
    return placa.isEmpty() ? sala(salaId).nome : placa;
}

// =======================================================================
// Nós (placas)
// =======================================================================
void Repositorio::registrarNo(const QString &chipId)
{
    if (chipId.trimmed().isEmpty()) return;

    QSqlQuery q(db);
    q.prepare("INSERT INTO nos (chip_id, ultimo_contato) VALUES (?, datetime('now')) "
              "ON CONFLICT (chip_id) DO UPDATE SET ultimo_contato = excluded.ultimo_contato");
    q.addBindValue(chipId);
    if (rodar(q)) vincularPorNome(chipId);
}

// Placa sem sala cujo ID é igual ao nome de uma sala livre: vincula as duas.
// Reproduz o comportamento antigo, em que o Change_ID dava à placa o nome da sala.
void Repositorio::vincularPorNome(const QString &chipId)
{
    QSqlQuery q(db);
    q.prepare("UPDATE nos SET sala_id = (SELECT id FROM salas WHERE nome = ?) "
              "WHERE chip_id = ? AND sala_id IS NULL "
              "AND EXISTS (SELECT 1 FROM salas s WHERE s.nome = ? "
              "            AND NOT EXISTS (SELECT 1 FROM nos n WHERE n.sala_id = s.id))");
    q.addBindValue(chipId);
    q.addBindValue(chipId);
    q.addBindValue(chipId);
    rodar(q);
}

void Repositorio::atualizarEstadoNo(const QString &chipId, const QString &estado, int temp)
{
    registrarNo(chipId);

    QSqlQuery q(db);
    q.prepare("UPDATE nos SET estado = ?, temp = ? WHERE chip_id = ?");
    q.addBindValue(estado);
    q.addBindValue(ouNulo(temp));
    q.addBindValue(chipId);
    rodar(q);
}

bool Repositorio::renomearNo(const QString &chipAntigo, const QString &chipNovo)
{
    if (chipNovo.trimmed().isEmpty() || chipAntigo == chipNovo) return false;

    db.transaction();

    // Um registro com o nome novo só pode ser sobra de uma placa antiga.
    QSqlQuery apaga(db);
    apaga.prepare("DELETE FROM nos WHERE chip_id = ?");
    apaga.addBindValue(chipNovo);
    rodar(apaga);

    QSqlQuery q(db);
    q.prepare("UPDATE nos SET chip_id = ? WHERE chip_id = ?");
    q.addBindValue(chipNovo);
    q.addBindValue(chipAntigo);
    if (!rodar(q)) {
        db.rollback();
        return false;
    }
    if (q.numRowsAffected() == 0) registrarNo(chipNovo);

    // Se o nome novo é de uma sala, a placa passa a atender essa sala.
    int salaId = salaPorNome(chipNovo, false);
    if (salaId >= 0) {
        QSqlQuery livre(db);
        livre.prepare("SELECT 1 FROM nos WHERE sala_id = ? AND chip_id <> ?");
        livre.addBindValue(salaId);
        livre.addBindValue(chipNovo);
        if (rodar(livre) && !livre.next()) vincularNo(chipNovo, salaId);
    }

    db.commit();
    return true;
}

bool Repositorio::vincularNo(const QString &chipId, int salaId)
{
    if (salaId >= 0) {
        // Uma sala tem no máximo uma placa: libera a sala antes.
        QSqlQuery libera(db);
        libera.prepare("UPDATE nos SET sala_id = NULL WHERE sala_id = ? AND chip_id <> ?");
        libera.addBindValue(salaId);
        libera.addBindValue(chipId);
        if (!rodar(libera)) return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE nos SET sala_id = ? WHERE chip_id = ?");
    q.addBindValue(ouNulo(salaId));
    q.addBindValue(chipId);
    return rodar(q) && q.numRowsAffected() > 0;
}

// =======================================================================
// Agendamentos
// =======================================================================
QList<Agendamento> Repositorio::agendamentos(int salaId, bool apenasAtivos) const
{
    QList<Agendamento> lista;
    QSqlQuery q(db);
    q.prepare(QString("SELECT id, sala_id, dia, hora, acao, temp, origem, ativo "
                      "FROM agendamentos WHERE sala_id = ? %1 ORDER BY id")
                  .arg(apenasAtivos ? "AND ativo = 1" : ""));
    q.addBindValue(salaId);
    if (!rodar(q)) return lista;

    while (q.next()) {
        Agendamento a;
        a.id     = q.value(0).toInt();
        a.salaId = q.value(1).toInt();
        a.dia    = q.value(2).toInt();
        a.hora   = q.value(3).toString();
        a.acao   = q.value(4).toString();
        a.temp   = q.value(5).isNull() ? -1 : q.value(5).toInt();
        a.origem = q.value(6).toString();
        a.ativo  = q.value(7).toInt() == 1;
        lista.append(a);
    }
    return lista;
}

int Repositorio::adicionarAgendamento(const Agendamento &a)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO agendamentos (sala_id, dia, hora, acao, temp, origem, ativo) "
              "VALUES (?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(a.salaId);
    q.addBindValue(a.dia);
    q.addBindValue(a.hora);
    q.addBindValue(a.acao);
    q.addBindValue(ouNulo(a.acao == "LIGAR" ? a.temp : -1));
    q.addBindValue(a.origem);
    q.addBindValue(a.ativo ? 1 : 0);
    if (!rodar(q)) return -1;
    return q.lastInsertId().toInt();
}

bool Repositorio::removerAgendamento(int agendamentoId)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM agendamentos WHERE id = ?");
    q.addBindValue(agendamentoId);
    return rodar(q) && q.numRowsAffected() > 0;
}

bool Repositorio::salaTemAgendamentos(int salaId) const
{
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM agendamentos WHERE sala_id = ? AND ativo = 1 LIMIT 1");
    q.addBindValue(salaId);
    return rodar(q) && q.next();
}

bool Repositorio::substituirAgendamentosSaci(const QMap<QString, QList<Agendamento>> &porNomeDeSala)
{
    db.transaction();

    if (!executar("DELETE FROM agendamentos WHERE origem = 'saci'")) {
        db.rollback();
        return false;
    }

    int total = 0;
    for (auto it = porNomeDeSala.begin(); it != porNomeDeSala.end(); ++it) {
        int salaId = salaPorNome(it.key(), true);
        if (salaId < 0) continue;

        QSqlQuery legado(db);
        legado.prepare("DELETE FROM agendamentos WHERE sala_id = ? AND origem = 'legado'");
        legado.addBindValue(salaId);
        rodar(legado);

        for (Agendamento a : it.value()) {
            a.salaId = salaId;
            a.origem = "saci";
            if (adicionarAgendamento(a) < 0) {
                db.rollback();
                return false;
            }
            total++;
        }
    }

    db.commit();
    qDebug() << "[Repositorio] SACI importado:" << total << "regras em" << porNomeDeSala.size() << "salas";
    return true;
}

// =======================================================================
// Configuração da sala no formato novo
//
// { "sala": "CI 101", "modelo": "Springer", "hash": "1a2b3c4d",
//   "codigos": { "LIGAR": "...", "T22": "..." },
//   "agenda":  [ [1, "07:00", "T22"], [1, "12:00", "DESLIGAR"] ] }
//
// O hash muda sempre que códigos ou agenda mudam: serve para o nó dizer
// qual configuração tem e a central saber se precisa reenviar.
// =======================================================================
QJsonObject Repositorio::configDaSala(int salaId) const
{
    Sala s = sala(salaId);

    QJsonObject codigos;
    const QMap<QString, QString> mapa = (s.modeloId >= 0) ? codigosDoModelo(s.modeloId)
                                                          : codigosDoUltimoModelo();
    for (auto it = mapa.begin(); it != mapa.end(); ++it) codigos[it.key()] = it.value();

    QList<Agendamento> regras = agendamentos(salaId, true);
    std::sort(regras.begin(), regras.end(), [](const Agendamento &a, const Agendamento &b) {
        return a.dia != b.dia ? a.dia < b.dia : a.hora < b.hora;
    });

    QJsonArray agenda;
    for (const Agendamento &a : regras) {
        QString comando = (a.acao == "LIGAR" && a.temp >= 0) ? QString("T%1").arg(a.temp) : a.acao;
        agenda.append(QJsonArray{a.dia, a.hora, comando});
    }

    QJsonObject conteudo;
    conteudo["codigos"] = codigos;
    conteudo["agenda"]  = agenda;
    QByteArray bruto = QJsonDocument(conteudo).toJson(QJsonDocument::Compact);
    QString hash = QString::fromLatin1(QCryptographicHash::hash(bruto, QCryptographicHash::Sha1).toHex().left(8));

    QJsonObject config;
    config["sala"]    = s.nome;
    config["modelo"]  = s.modeloId >= 0 ? QJsonValue(modeloNome(s.modeloId)) : QJsonValue();
    config["hash"]    = hash;
    config["codigos"] = codigos;
    config["agenda"]  = agenda;
    return config;
}

QString Repositorio::modeloNome(int modeloId) const
{
    QSqlQuery q(db);
    q.prepare("SELECT nome FROM modelos WHERE id = ?");
    q.addBindValue(modeloId);
    return (rodar(q) && q.next()) ? q.value(0).toString() : QString();
}

// =======================================================================
// Conversões de nomes antigos
// =======================================================================
QString Repositorio::comandoCanonico(const QString &nomeAntigo)
{
    QString t = nomeAntigo.trimmed().toUpper();
    if (t == "LIGAR" || t == "DESLIGAR") return t;

    static const QRegularExpression re("^T?(\\d{2})$");
    QRegularExpressionMatch m = re.match(t);
    if (m.hasMatch()) {
        int temp = m.captured(1).toInt();
        if (temp >= 16 && temp <= 30) return QString("T%1").arg(temp);
    }
    return QString();
}

QString Repositorio::comandoFirmware(const QString &canonico)
{
    if (canonico == "LIGAR")    return "Ligar";
    if (canonico == "DESLIGAR") return "Desligar";
    if (canonico.startsWith('T')) return canonico.mid(1);
    return canonico;
}

int Repositorio::diaParaNumero(const QString &dia)
{
    QString t = dia.trimmed().left(3);
    t.replace(QChar(0x00E1), u'a');   // "Sáb" -> "Sab"
    for (int i = 0; i < DIAS.size(); ++i) {
        if (DIAS[i].compare(t, Qt::CaseInsensitive) == 0) return i;
    }
    return -1;
}

QString Repositorio::diaParaTexto(int dia)
{
    return (dia >= 0 && dia < DIAS.size()) ? DIAS[dia] : QString();
}
