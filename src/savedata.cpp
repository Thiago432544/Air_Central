#include "savedata.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

SaveData::SaveData(Repositorio *repositorio, QObject *parent)
    : QObject(parent), repo(repositorio)
{
}

// =======================================================================
// Códigos IR
// =======================================================================
void SaveData::save_data(const QJsonObject newData)
{
    bool foiModificado = false;

    for (auto it = newData.constBegin(); it != newData.constEnd(); ++it) {
        QString comando = Repositorio::comandoCanonico(it.key());
        QString codigo  = it.value().toString();

        if (comando.isEmpty() || codigo.isEmpty()) {
            qDebug() << "Comando ou código inválido, ignorado:" << it.key();
            continue;
        }

        if (repo->codigoJaExiste(comando, codigo)) {
            qDebug() << "O código recebido já estava salvo. Ignorado.";
            continue;
        }

        int modeloId = repo->adicionarCodigoProximoModelo(comando, codigo);
        if (modeloId >= 0) {
            qDebug() << "Código de" << comando << "salvo no modelo" << modeloId;
            foiModificado = true;
        }
    }

    if (foiModificado) emit codes_size_changed();
}

void SaveData::delete_data(const QString &comando)
{
    if (repo->apagarUltimoCodigo(Repositorio::comandoCanonico(comando))) {
        qDebug() << "Último código de" << comando << "apagado.";
    } else {
        qDebug() << "O comando" << comando << "já está sem códigos.";
    }
    emit codes_size_changed();
}

void SaveData::delete_all_data()
{
    repo->apagarTodosCodigos();
    qDebug() << "Todos os códigos IR apagados.";
    emit codes_size_changed();
}

int SaveData::get_command_count(const QString &comando)
{
    return repo->contarCodigos(Repositorio::comandoCanonico(comando));
}

// =======================================================================
// Agendamentos (a tela identifica a sala pelo ID da placa)
// =======================================================================
void SaveData::adicionar_regra(QString idEsp, QString hora, QString acao, QString temp, QString dia)
{
    int salaId = repo->salaDoDispositivo(idEsp, true);
    if (salaId < 0) return;

    Agendamento a;
    a.salaId = salaId;
    a.dia    = Repositorio::diaParaNumero(dia);
    a.hora   = hora;
    a.acao   = Repositorio::comandoCanonico(acao);
    bool ok  = false;
    int t    = temp.toInt(&ok);
    a.temp   = ok ? t : -1;
    a.origem = "manual";

    if (a.dia < 0 || (a.acao != "LIGAR" && a.acao != "DESLIGAR")) {
        qDebug() << "Regra inválida, não salva:" << dia << hora << acao;
        return;
    }

    if (repo->adicionarAgendamento(a) >= 0) {
        qDebug() << "Regra adicionada para" << idEsp << ":" << dia << hora << acao << temp;
    }
}

void SaveData::remover_regra(QString idEsp, int index)
{
    int salaId = repo->salaDoDispositivo(idEsp, false);
    if (salaId < 0) return;

    // O índice é a posição na lista que obter_regras() devolveu (ordem de criação).
    const QList<Agendamento> regras = repo->agendamentos(salaId);
    if (index >= 0 && index < regras.size()) {
        repo->removerAgendamento(regras[index].id);
        qDebug() << "Regra no índice" << index << "removida de" << idEsp;
    }
}

QString SaveData::obter_regras(QString idEsp)
{
    int salaId = repo->salaDoDispositivo(idEsp, false);
    if (salaId < 0) return "[]";

    QJsonArray lista;
    for (const Agendamento &a : repo->agendamentos(salaId)) {
        QJsonObject r;
        r["dia"]  = Repositorio::diaParaTexto(a.dia);
        r["hora"] = a.hora;
        r["acao"] = Repositorio::comandoFirmware(a.acao);   // "Ligar" / "Desligar"
        r["temp"] = a.temp >= 0 ? QString::number(a.temp) : QString("--");
        lista.append(r);
    }
    return QString(QJsonDocument(lista).toJson(QJsonDocument::Compact));
}

// =======================================================================
// Importação do SACI
// O script continua gerando JSON; o resultado vai para o banco e só as
// regras de origem 'saci' são trocadas (as manuais ficam).
// =======================================================================
void SaveData::importar_saci(const QString &centroId, int tempPadrao)
{
    QString script = QCoreApplication::applicationDirPath() + "/saci_ci.py";
    if (!QFile::exists(script)) script = "saci_ci.py";   // pasta atual (como antes)

    QString pasta = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(pasta);
    QString saida = pasta + "/saci_importacao.json";
    QFile::remove(saida);

    qDebug() << "Disparando automação do script Python" << script;

    QProcess *process = new QProcess(this);
    QStringList argumentos;
    argumentos << script
               << "--id"   << centroId
               << "--temp" << QString::number(tempPadrao)
               << "--out"  << saida;

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, saida](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
                    carregar_importacao_saci(saida);
                } else {
                    qDebug() << "Erro ao rodar o script Python. Código de saída:" << exitCode;
                    qDebug() << "LOG DE ERRO DO PYTHON:" << process->readAllStandardError();
                }
                process->deleteLater();
            });

#ifdef Q_OS_WIN
    QString pythonCmd = "python";
#else
    QString pythonCmd = "python3";
#endif

    process->start(pythonCmd, argumentos);
}

void SaveData::carregar_importacao_saci(const QString &arquivo)
{
    QFile f(arquivo);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Saída do SACI não encontrada:" << arquivo;
        return;
    }
    QJsonObject raiz = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    QMap<QString, QList<Agendamento>> porSala;
    for (auto it = raiz.begin(); it != raiz.end(); ++it) {
        QList<Agendamento> regras;
        for (const QJsonValue &valor : it.value().toArray()) {
            QJsonObject r = valor.toObject();

            Agendamento a;
            a.dia   = Repositorio::diaParaNumero(r["dia"].toString());
            a.hora  = r["hora"].toString();
            a.acao  = Repositorio::comandoCanonico(r["acao"].toString());
            bool ok = false;
            int t   = r["temp"].toString().toInt(&ok);
            a.temp  = ok ? t : -1;

            if (a.dia < 0 || (a.acao != "LIGAR" && a.acao != "DESLIGAR")) continue;
            regras.append(a);
        }
        porSala.insert(it.key(), regras);
    }

    if (repo->substituirAgendamentosSaci(porSala)) {
        emit agendamentos_atualizados();
    }
}
