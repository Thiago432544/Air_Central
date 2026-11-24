#include "senddata.h"

SendData::SendData(QSerialPort *portaSerial, Repositorio *repositorio, QObject *parent)
    : QObject(parent), serial(portaSerial), repo(repositorio)
{
    timerFila = new QTimer(this);
    connect(timerFila, &QTimer::timeout, this, &SendData::process_data);
}

void SendData::send_command_status(QString idEsp, QString status)
{
    QJsonObject cmd;
    cmd["command"] = "Dispatch";
    cmd["id"]      = idEsp;
    cmd["status"]  = status;

    cmd["name"]    = (status == "Ligado") ? "Ligar" : "Desligar";

    send_reliable(cmd);
}

void SendData::send_command_temp(QString idEsp, QString temperatura)
{
    QJsonObject cmd;
    cmd["command"] = "Dispatch";
    cmd["id"]      = idEsp;
    // Número, não texto: o nó lê "temp" com as<int>(), que devolve 0 para texto.
    cmd["temp"]    = temperatura.toInt();

    // O nome continua texto: é o nome do arquivo do código na flash ("22").
    cmd["name"]    = temperatura;

    send_reliable(cmd);
}

void SendData::require_espID_change(const QString idEsp, const QString newId)
{
    QJsonObject jsonRequest;
    jsonRequest["command"] = "Change_ID";
    jsonRequest["id"] = idEsp;
    jsonRequest["new_id"] = newId;

    send_data(jsonRequest);

    // A placa passa a se chamar newId; se for o nome de uma sala, fica vinculada a ela.
    repo->renomearNo(idEsp, newId);
}

void SendData::requireEspsId()
{
    QJsonObject jsonRequest;
    jsonRequest["command"] = "Require_IDs";
    send_data(jsonRequest);
    qDebug() << "Classe SendData requisitou IDs";
}

void SendData::require_ir_read()
{
    QJsonObject jsonRequest;
    jsonRequest["command"] = "Require_IR";
    send_data(jsonRequest);
    qDebug() << "Classe SendData requisitou leitura de infravermelho";
}

QJsonObject SendData::build_time_command() const
{
    // Epoch UTC; o nó converte para o horário local com a TZ configurada nele.
    QJsonObject cmdTime;
    cmdTime["command"] = "Sync_Time";
    cmdTime["timestamp"] = QDateTime::currentSecsSinceEpoch();
    return cmdTime;
}

void SendData::send_time()
{
    // Depois de uma queda de energia todos os nós pedem a hora ao mesmo tempo;
    // um único broadcast a cada 10 s atende todos.
    qint64 agora = QDateTime::currentSecsSinceEpoch();
    if (agora - ultimoEnvioHora < 10) {
        return;
    }
    ultimoEnvioHora = agora;

    send_data(build_time_command());
}

void SendData::send_data(QJsonObject jsonCommand)
{
    QJsonDocument doc(jsonCommand);
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    data.append('\n');

    if (serial && serial->isOpen()) {
        serial->write(data);
        qDebug() << "Classe SendData disparou:" << data;
    } else {
        qDebug() << "Erro: Tentando enviar";
        emit errorOccurred("Falha ao enviar: A porta serial não está conectada ou aberta.");
    }
}

// =======================================================================
// Entrega confiável (tarefa 2)
// =======================================================================
// Hoje só numera e envia: a mensagem sai com "seq" e o nó devolve o mesmo
// seq no Ack. Ainda não há espera, reenvio nem desistência.
void SendData::send_reliable(QJsonObject jsonCommand)
{
    int seq = proximoSeq++;
    jsonCommand["seq"] = seq;

    // TODO(tarefa 2): guardar a mensagem em pendentes[seq] e armar um timeout
    // (ex.: QTimer de 2 s por mensagem, ou um timer único que varre
    // pendentes). Se o Ack não chegar a tempo, reenviar a MESMA mensagem, com
    // o MESMO seq (é assim que o nó reconhece a duplicata). Depois de N
    // tentativas, tirar de pendentes e emitir
    // deliveryFailed(id, command).
    send_data(jsonCommand);
}

void SendData::on_ack(QString de, int seq)
{
    // TODO(tarefa 2): se seq estiver em pendentes, a entrega foi confirmada:
    // remover de pendentes e parar o timeout dela. Um Ack de seq que não
    // está em pendentes (atrasado ou repetido) só é ignorado.
    qDebug() << "Ack recebido de" << de << "seq" << seq;
}

void SendData::sinc_esp_data()
{
    filaDeMensagens.clear();

    // ---------------------------------------------------------
    // PASSO 1: Empacotar a Hora Atual (Timestamp Unix)
    // ---------------------------------------------------------
    filaDeMensagens.enqueue(build_time_command());

    // ---------------------------------------------------------
    // PASSO 2: Códigos IR e agendamentos de cada sala, lidos do banco
    // ---------------------------------------------------------
    // Vai para as salas com placa vinculada ou com agendamentos ativos.
    // Cada sala recebe só os códigos do modelo dela; sala sem modelo definido
    // recebe os do último modelo cadastrado (como era antes do banco).
    const QList<Sala> salas = repo->salas();
    for (const Sala &sala : salas) {
        QString placa = repo->placaDaSala(sala.id);
        if (placa.isEmpty() && !repo->salaTemAgendamentos(sala.id)) continue;

        QString idEsp = placa.isEmpty() ? sala.nome : placa;
        QJsonObject config = repo->configDaSala(sala.id);

        if (sala.modeloId < 0) {
            qDebug() << "Sala" << sala.nome << "sem modelo definido: usando os códigos do último modelo.";
        }

        // Agenda que usa um código que o modelo não tem: o nó não vai achar o
        // arquivo na hora de disparar.
        const QJsonObject codigos = config["codigos"].toObject();
        for (const QJsonValue &valor : config["agenda"].toArray()) {
            QString comando = valor.toArray().at(2).toString();
            if (!codigos.contains(comando)) {
                qWarning() << "Sala" << sala.nome << ": a agenda usa" << comando
                           << "mas o modelo não tem esse código.";
            }
        }

        // A config inteira vai como uma transação: o nó só troca a config
        // atual quando chegar o Config_Fim com todas as partes.
        for (const QJsonObject &mensagem : montarTransacaoConfig(idEsp, config)) {
            filaDeMensagens.enqueue(mensagem);
        }
    }

    // ---------------------------------------------------------
    // PASSO 3: Iniciar o Disparo
    // ---------------------------------------------------------
    if (!filaDeMensagens.isEmpty()) {
        qDebug() << "Iniciando envio de" << filaDeMensagens.size() << "pacotes...";

        emit syncStarted(filaDeMensagens.size());

        timerFila->start(350);

    } else {
        emit syncFinished();
    }
}

QList<QJsonObject> SendData::montarTransacaoConfig(const QString &idEsp, const QJsonObject &config)
{
    // Até 40 agendamentos por Config_Agenda: cada item tem uns 20 bytes, então
    // a parte fica bem abaixo da linha de 4.096 bytes da ponte.
    const int ITENS_POR_PARTE = 40;

    const QString hash = config["hash"].toString();
    const QJsonObject codigos = config["codigos"].toObject();
    const QJsonArray agenda = config["agenda"].toArray();
    const int partesAgenda = (agenda.size() + ITENS_POR_PARTE - 1) / ITENS_POR_PARTE;

    QList<QJsonObject> mensagens;

    QJsonObject inicio;
    inicio["command"] = "Config_Inicio";
    inicio["id"]      = idEsp;
    inicio["hash"]    = hash;
    inicio["codigos"] = codigos.size();
    inicio["agendas"] = partesAgenda;
    mensagens.append(inicio);

    // O firmware ainda usa os nomes antigos ("Ligar", "22") como nome de arquivo.
    for (auto it = codigos.begin(); it != codigos.end(); ++it) {
        QJsonObject codigo;
        codigo["command"] = "Config_Codigo";
        codigo["id"]      = idEsp;
        codigo["hash"]    = hash;
        codigo["nome"]    = Repositorio::comandoFirmware(it.key());
        codigo["dados"]   = it.value().toString();

        // A ponte corta linhas maiores que 4.096 bytes; o nó descartaria a
        // parte e a transação ficaria incompleta.
        int tamanho = QJsonDocument(codigo).toJson(QJsonDocument::Compact).size() + 1;
        if (tamanho > 4096) {
            qWarning() << "Código" << it.key() << "de" << idEsp << "tem" << tamanho
                       << "bytes e não cabe na linha da ponte (4096).";
        }
        mensagens.append(codigo);
    }

    // agenda: [ [dia, "HH:MM", "COMANDO"], ... ], com o comando no nome do firmware
    for (int parte = 0; parte < partesAgenda; ++parte) {
        QJsonArray itens;
        for (int i = parte * ITENS_POR_PARTE; i < qMin(agenda.size(), (parte + 1) * ITENS_POR_PARTE); ++i) {
            QJsonArray regra = agenda.at(i).toArray();
            itens.append(QJsonArray{regra.at(0).toInt(), regra.at(1).toString(),
                                    Repositorio::comandoFirmware(regra.at(2).toString())});
        }
        QJsonObject pedaco;
        pedaco["command"] = "Config_Agenda";
        pedaco["id"]      = idEsp;
        pedaco["hash"]    = hash;
        pedaco["parte"]   = parte + 1;
        pedaco["itens"]   = itens;
        mensagens.append(pedaco);
    }

    QJsonObject fim;
    fim["command"] = "Config_Fim";
    fim["id"]      = idEsp;
    fim["hash"]    = hash;
    mensagens.append(fim);

    return mensagens;
}

void SendData::process_data()
{
    if (filaDeMensagens.isEmpty()) {
        timerFila->stop();
        qDebug() << "Sincronização concluída com sucesso!";

        emit syncFinished();
        return;
    }

    QJsonObject pacote = filaDeMensagens.dequeue();
    send_data(pacote);

    emit syncProgress(filaDeMensagens.size());
}