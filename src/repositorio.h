#ifndef REPOSITORIO_H
#define REPOSITORIO_H

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// =======================================================================
// Único ponto de acesso aos dados da central (SQLite).
//
// Nomes de comando no banco: LIGAR, DESLIGAR, T16..T30.
// Dias da semana: 0 = Dom ... 6 = Sab.
// As conversões para os nomes antigos ("Ligar", "22", "Seg") ficam aqui,
// em um lugar só, enquanto a tela e o firmware ainda usam esses nomes.
// =======================================================================

struct Modelo {
    int id = -1;
    QString nome;
    QString marca;
    QString formato;
};

struct Sala {
    int id = -1;
    QString nome;
    QString bloco;
    int modeloId = -1;      // -1 = modelo não definido
    int tempPadrao = 22;
};

struct Agendamento {
    int id = -1;
    int salaId = -1;
    int dia = 0;            // 0..6
    QString hora;           // "HH:MM"
    QString acao;           // LIGAR | DESLIGAR
    int temp = -1;          // -1 = sem temperatura
    QString origem = "manual";
    bool ativo = true;
};

class Repositorio : public QObject
{
    Q_OBJECT

public:
    explicit Repositorio(QObject *parent = nullptr);
    ~Repositorio();

    // Abre (ou cria) o banco. Caminho vazio = pasta de dados do app.
    // Na criação, importa codes.json e agendamentos.json se existirem.
    bool abrir(const QString &caminho = QString());
    QString caminhoBanco() const { return caminho; }

    // --- Modelos -------------------------------------------------------
    // As funções Q_INVOKABLE são chamadas direto pela tela de modelos (QML).
    QList<Modelo> modelos() const;
    Q_INVOKABLE int criarModelo(const QString &nome, const QString &marca = QString());  // -1 = nome vazio ou repetido
    Q_INVOKABLE bool renomearModelo(int modeloId, const QString &nome, const QString &marca);
    Q_INVOKABLE bool apagarModelo(int modeloId);

    // Para o QML: [{id, nome, marca, formato, codigos, salas}, ...]
    // "codigos" = quantos dos 17 comandos já foram capturados;
    // "salas" = quantas salas usam o modelo.
    Q_INVOKABLE QVariantList listarModelos() const;

    // Para o QML: {"LIGAR": true, "DESLIGAR": false, "T16": ..., "T30": ...}
    Q_INVOKABLE QVariantMap comandosDoModelo(int modeloId) const;

    // Os 17 comandos, na ordem da tela: LIGAR, DESLIGAR, T16..T30.
    static QStringList comandosValidos();

    // --- Códigos IR ----------------------------------------------------
    bool salvarCodigo(int modeloId, const QString &comando, const QString &dados);
    Q_INVOKABLE bool apagarCodigo(int modeloId, const QString &comando);
    QMap<QString, QString> codigosDoModelo(int modeloId) const;   // comando -> dados
    bool codigoJaExiste(const QString &comando, const QString &dados) const;

    // Compatibilidade com a tela atual, que grava "um código por modelo"
    // sem dizer qual modelo: o código vai para o primeiro modelo que ainda
    // não tem esse comando ("Modelo 1", "Modelo 2", ... criados sob demanda).
    int adicionarCodigoProximoModelo(const QString &comando, const QString &dados);
    bool apagarUltimoCodigo(const QString &comando);
    bool apagarTodosCodigos();
    int contarCodigos(const QString &comando) const;               // nº de modelos com o comando
    QMap<QString, QString> codigosDoUltimoModelo() const;          // comportamento antigo (last())

    // --- Salas ---------------------------------------------------------
    QList<Sala> salas() const;
    Sala sala(int salaId) const;
    int salaPorNome(const QString &nome, bool criar = true);
    bool definirModeloSala(int salaId, int modeloId);              // modeloId -1 = nenhum

    // Para o QML, que identifica a sala pelo nome da placa (model.name):
    // {id, nome} do modelo da sala, ou {id: -1, nome: ""} se não houver.
    Q_INVOKABLE QVariantMap modeloDoDispositivo(const QString &idDispositivo);
    Q_INVOKABLE bool definirModeloDoDispositivo(const QString &idDispositivo, int modeloId);

    // Sala atendida por um dispositivo: pela placa vinculada ou, se não houver,
    // pela sala com o mesmo nome (hoje o Change_ID dá à placa o nome da sala).
    int salaDoDispositivo(const QString &idDispositivo, bool criar = true);

    // ID para endereçar a sala na rede: o chip_id da placa vinculada ou,
    // sem placa, o próprio nome da sala.
    QString destinoDaSala(int salaId) const;
    QString placaDaSala(int salaId) const;                         // vazio = sem placa

    // --- Nós (placas) --------------------------------------------------
    void registrarNo(const QString &chipId);
    void atualizarEstadoNo(const QString &chipId, const QString &estado, int temp);
    bool renomearNo(const QString &chipAntigo, const QString &chipNovo);
    bool vincularNo(const QString &chipId, int salaId);           // salaId -1 = desvincular

    // --- Agendamentos --------------------------------------------------
    QList<Agendamento> agendamentos(int salaId, bool apenasAtivos = false) const;  // ordem de criação
    int adicionarAgendamento(const Agendamento &agenda);
    bool removerAgendamento(int agendamentoId);
    bool salaTemAgendamentos(int salaId) const;

    // Troca todas as regras 'saci' pelas novas, numa transação. Nas salas que
    // aparecem na importação, as regras 'legado' também saem (vieram do SACI
    // antigo na prática); regras 'manual' nunca são apagadas.
    bool substituirAgendamentosSaci(const QMap<QString, QList<Agendamento>> &porNomeDeSala);

    // --- Configuração da sala (formato novo, para o firmware) -----------
    QJsonObject configDaSala(int salaId) const;

    // --- Conversões de nomes antigos -----------------------------------
    static QString comandoCanonico(const QString &nomeAntigo);     // "Ligar" -> LIGAR, "22" -> T22
    static QString comandoFirmware(const QString &canonico);       // LIGAR -> "Ligar", T22 -> "22"
    static int diaParaNumero(const QString &dia);                  // "Seg" -> 1, -1 se inválido
    static QString diaParaTexto(int dia);                          // 1 -> "Seg"

signals:
    // Modelos, códigos ou o modelo de alguma sala mudaram: as telas recarregam.
    void modelosAlterados();

private:
    bool criarSchema();
    void migrarJsonLegado();
    bool executar(const QString &sql);
    int modeloPorNome(const QString &nome, bool criar);
    QString modeloNome(int modeloId) const;
    void vincularPorNome(const QString &chipId);

    QSqlDatabase db;
    QString caminho;
    QString nomeConexao;
};

#endif // REPOSITORIO_H
