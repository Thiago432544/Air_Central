#ifndef SAVEDATA_H
#define SAVEDATA_H

#include <QObject>
#include <QJsonObject>
#include <QString>

#include "repositorio.h"

// =======================================================================
// Adaptador entre as telas QML atuais e o Repositorio (SQLite).
// Mantém as mesmas funções que o QML já chama; os dados agora vivem no banco.
// Quando as telas passarem a falar com o Repositorio direto, esta classe sai.
// =======================================================================
class SaveData : public QObject
{
    Q_OBJECT

public:
    explicit SaveData(Repositorio *repositorio, QObject *parent = nullptr);

    // Código IR capturado: { "<comando antigo>": "<raw>" }
    void save_data(const QJsonObject data);

    Q_INVOKABLE void delete_data(const QString &comando);
    Q_INVOKABLE void delete_all_data();
    Q_INVOKABLE int get_command_count(const QString &comando);

    Q_INVOKABLE void adicionar_regra(QString idEsp, QString hora, QString acao, QString temp, QString dia);
    Q_INVOKABLE void remover_regra(QString idEsp, int index);
    Q_INVOKABLE QString obter_regras(QString idEsp);

    Q_INVOKABLE void importar_saci(const QString &centroId = "CI", int tempPadrao = 22);

signals:
    void codes_size_changed();
    void agendamentos_atualizados();

private:
    void carregar_importacao_saci(const QString &arquivo);

    Repositorio *repo;
};

#endif // SAVEDATA_H
