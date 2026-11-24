#include "receivedata.h"

ReceiveData::ReceiveData(SaveData *saveData, Repositorio *repositorio, QObject *parent)
    : QObject(parent), saveData(saveData), repo(repositorio)
{
    serialPort = new QSerialPort(this);

    connect(serialPort, &QSerialPort::readyRead,
            this, &ReceiveData::read_data);
}

QStringList ReceiveData::list_ports() {
    QStringList nomesDasPortas;

    const auto portas = QSerialPortInfo::availablePorts();

    nomesDasPortas.append("--");
    for (const QSerialPortInfo &porta : portas) {
        nomesDasPortas.append(porta.portName());
    }
    return nomesDasPortas;
}

bool ReceiveData::conectar(QString nomePorta)
{
    if (serialPort == nullptr) {
        serialPort = new QSerialPort(this);
    }

    if (serialPort->isOpen()) {
        serialPort->close();
        qDebug() << "Desconectando da porta";
        return false;
    } else {
        serialPort->setPortName(nomePorta);

        serialPort->setBaudRate(QSerialPort::Baud115200);

        if (serialPort->open(QIODevice::ReadWrite)) {
            qDebug() << "Conectado com sucesso na porta:" << nomePorta;
            return true;
        } else {
            qDebug() << "Erro ao iniciar!" << serialPort->errorString();
            return false;
        }
    }
}

void ReceiveData::read_data()
{
    buffer.append(serialPort->readAll());

    while (buffer.contains('\n')) {
        int indexQuebra = buffer.indexOf('\n');

        QByteArray linha = buffer.left(indexQuebra).trimmed();

        buffer.remove(0, indexQuebra + 1);

        if (linha.isEmpty()) {
            continue;
        }

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(linha, &error);

        if (doc.isNull() || error.error != QJsonParseError::NoError || !doc.isObject()) {
            qDebug() << "Dado recebido não é um JSON válido ou está corrompido:" << linha;
            continue;
        }

        QJsonObject jsonObject = doc.object();

        decode_data(jsonObject);
    }
}

void ReceiveData::decode_data(QJsonObject jsonObject)
{
    QString messageType = jsonObject["command"].toString();

    if (messageType == "Resposta_ID") {

        QString idEsp = jsonObject["id"].toString();
        qDebug() << "Novo ESP descoberto na rede Mesh:" << idEsp;
        repo->registrarNo(idEsp);

        emit newDeviceDetected(idEsp);
    }

    else if (messageType == "new_code") {
        // Captura pedida pela tela de modelos: grava no modelo escolhido.
        if (modeloEsperado >= 0) {
            QString code = jsonObject["code"].toString();
            int modelo = modeloEsperado;
            QString comando = comandoEsperado;
            modeloEsperado = -1;
            comandoEsperado.clear();

            if (code.isEmpty() || !repo->salvarCodigo(modelo, comando, code)) {
                qDebug() << "Falha ao gravar o código IR de" << comando << "no modelo" << modelo;
                emit capturaFalhou("Não foi possível gravar o código.");
                return;
            }
            qDebug() << "Código de" << comando << "gravado no modelo" << modelo;
            emit codigoCapturado(modelo, comando);
            return;
        }

        if (waitedCommand.isEmpty()) {
            qDebug() << "Recebi um código IR, mas não estou em modo de cadastro. Ignorando.";
            return;
        }

        QString code = jsonObject["code"].toString();

        QJsonObject novoDado;
        novoDado[waitedCommand] = code;

        saveData->save_data(novoDado);

        waitedCommand = "";
        qDebug() << "Código salvo com sucesso!";
    }

    else if (messageType == "status_update") {

        QString idEsp = jsonObject["id"].toString();
        QString status = jsonObject["status"].toString();
        // O nó manda "temp" como número; toString() num número devolve "".
        int temp = jsonObject["temp"].toVariant().toInt();
        qDebug() << "Atualização:" << idEsp << status << temp;
        repo->atualizarEstadoNo(idEsp, status, temp);

        emit statusUpdated(idEsp, status, temp);
    }

    else if (messageType == "Need_Time") {
        qDebug() << "ESP sem hora válida pediu sincronização:" << jsonObject["id"].toString();
        emit timeRequested();
    }
}

void ReceiveData::set_waited_capture(int modeloId, QString comando)
{
    waitedCommand.clear();
    modeloEsperado = modeloId;
    comandoEsperado = comando;
    qDebug() << "ReceiveData aguardando código de" << comando << "para o modelo" << modeloId;
}

void ReceiveData::cancel_waited_capture()
{
    modeloEsperado = -1;
    comandoEsperado.clear();
}

void ReceiveData::set_waited_command(QString commandType)
{
    modeloEsperado = -1;
    comandoEsperado.clear();
    waitedCommand = commandType;
    qDebug() << "ReceiveData agora está aguardando um código para:" << waitedCommand;
}