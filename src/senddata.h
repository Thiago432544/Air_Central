#ifndef SENDDATA_H
#define SENDDATA_H

#include <QObject>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QJsonObject>
#include <QJsonDocument>
#include <QQueue>
#include <QDebug>
#include <QJsonArray>
#include <QFile>
#include <QTimer>
#include <QDate>
#include <QHash>

#include "repositorio.h"

class SendData : public QObject
{
    Q_OBJECT
public:
    explicit SendData(QSerialPort *portaSerial, Repositorio *repositorio, QObject *parent = nullptr);

    void send_data(QJsonObject jsonCommand);

    // Envio com confirmação (tarefa 2): numera a mensagem com "seq" e a envia.
    void send_reliable(QJsonObject jsonCommand);

    Q_INVOKABLE void sinc_esp_data();

    // Config completa de uma sala como transação (pendência 5):
    // Config_Inicio, um Config_Codigo por código, Config_Agenda (em partes)
    // e Config_Fim. "config" é o que Repositorio::configDaSala devolve.
    static QList<QJsonObject> montarTransacaoConfig(const QString &idEsp, const QJsonObject &config);
    Q_INVOKABLE void send_time();
    Q_INVOKABLE void send_command_status(QString idEsp, QString status);
    Q_INVOKABLE void send_command_temp(QString idEsp, QString temperatura);
    Q_INVOKABLE void requireEspsId();
    Q_INVOKABLE void require_ir_read();
    Q_INVOKABLE void require_espID_change(const QString idEsp, const QString newId);

public slots:
    // Chamado quando chega {"command":"Ack","de":...,"seq":...} de um nó.
    void on_ack(QString de, int seq);

signals:
    void errorOccurred(QString errorMessage);
    // Emitido quando uma mensagem confiável esgota as tentativas sem Ack.
    void deliveryFailed(QString idEsp, QString command);
    void syncStarted(int totalPacotes);
    void syncProgress(int pacotesRestantes);
    void syncFinished();

private slots:
    void process_data();

private:
    QSerialPort *serial;
    Repositorio *repo;

    QJsonObject build_time_command() const;

    QTimer *timerFila;
    qint64 ultimoEnvioHora = 0;
    QQueue<QJsonObject> filaDeMensagens;

    // Entrega confiável (tarefa 2)
    int proximoSeq = 1;
    QHash<int, QJsonObject> pendentes; // seq -> mensagem ainda sem Ack
};

#endif // SENDDATA_H