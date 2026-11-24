#ifndef RECEIVEDATA_H
#define RECEIVEDATA_H

#include <QObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDebug>
#include <QSerialPort>
#include <qserialportinfo.h>

#include "savedata.h"

class ReceiveData : public QObject
{
    Q_OBJECT

public:
    explicit ReceiveData(SaveData *saveData, Repositorio *repositorio, QObject *parent = nullptr);

    // CORREÇÃO: Função restaurada para o main.cpp conseguir acessar a porta!
    QSerialPort* getSerialPort() { return serialPort; }

    Q_INVOKABLE bool conectar(QString nomePorta);

    Q_INVOKABLE QStringList list_ports();

public slots:
    // Tela antiga: grava no "próximo modelo" (comando no formato antigo, ex. "Ligar").
    void set_waited_command(QString commandType);

    // Tela de modelos: o próximo código IR que chegar vai para este modelo e
    // comando (formato do banco: LIGAR, DESLIGAR, T16..T30).
    void set_waited_capture(int modeloId, QString comando);
    void cancel_waited_capture();

signals:
    void newDeviceDetected(QString idEsp);
    void statusUpdated(QString idEsp, QString status, int temp);
    void timeRequested();
    // Um código IR foi gravado pela captura da tela de modelos.
    void codigoCapturado(int modeloId, QString comando);
    void capturaFalhou(QString motivo);
    // Mensagens novas do formato v1 (docs/REDE.md na branch rede/base-firmware)
    void ackReceived(QString de, int seq);
    void pongReceived(QString de, int seq);

private slots:
    void read_data();
    void decode_data(QJsonObject obj);

private:
    QSerialPort *serialPort;
    QByteArray buffer;
    QString waitedCommand = "";
    int modeloEsperado = -1;          // -1 = nenhuma captura da tela de modelos pendente
    QString comandoEsperado;

    SaveData *saveData;
    Repositorio *repo;
};

#endif // RECEIVEDATA_H