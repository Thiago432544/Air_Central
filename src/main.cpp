#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>

#include "receivedata.h"
#include "senddata.h"
#include "savedata.h"
#include "repositorio.h"

int main(int argc, char *argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);
    app.setApplicationName("Air_Central");   // define a pasta de dados (onde fica o banco)
    app.setWindowIcon(QIcon(":/qt/qml/Central_Ar_Condicionado/qml/icon.svg"));
    QQuickStyle::setStyle("Basic");

    // 1. Instanciamos apenas as classes que conversam com a Interface
    // Banco SQLite na pasta de dados do app. Na primeira execução importa
    // codes.json e agendamentos.json da versão antiga, se estiverem na pasta.
    Repositorio repositorio;
    if (!repositorio.abrir()) {
        qCritical() << "Sem banco de dados: códigos e agendamentos não serão salvos.";
    }

    SaveData saveData(&repositorio);
    ReceiveData receiveData(&saveData, &repositorio);
    SendData sendData(receiveData.getSerialPort(), &repositorio);

    // Um nó que reiniciou (sem RTC) pede a hora; a central responde na hora.
    QObject::connect(&receiveData, &ReceiveData::timeRequested,
                     &sendData, &SendData::send_time);

    // Entrega confiável (tarefa 2): o Ack de um nó confirma um envio pendente.
    QObject::connect(&receiveData, &ReceiveData::ackReceived,
                     &sendData, &SendData::on_ack);

    QQmlApplicationEngine engine;

    // 2. Expondo as instâncias para o QML
    engine.rootContext()->setContextProperty("receiveData", &receiveData);
    engine.rootContext()->setContextProperty("sendData", &sendData);
    engine.rootContext()->setContextProperty("saveData", &saveData);
    engine.rootContext()->setContextProperty("repositorio", &repositorio);

    // Carrega o arquivo QML (Padrão Qt 6)
    engine.loadFromModule("Central_Ar_Condicionado", "Main");

    return app.exec();
}
