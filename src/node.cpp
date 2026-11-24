#include "node.h"

// Inicialização dos objetos globais
painlessMesh mesh;
Scheduler userScheduler;
IRsend irsend(IR_SEND_PIN);

std::vector<Agendamento> listaAgendamentos;

String myID;
bool powerStatus = false;
int temperaturaAtual = 25;
int temperaturaAlvo = 25;

// Entrega confiável (tarefa 2): "seq" do último Dispatch executado, para não
// disparar o IR de novo quando a central reenviar a mesma mensagem.
long ultimoSeqExecutado = -1;
// =======================================================================
// Config completa e atômica (pendência 5): a lógica fica em
// transacao_config.h; aqui só a ponte entre ela e a LittleFS.
// =======================================================================
class FlashLittleFS : public Armazenamento {
public:
  std::vector<std::string> listar() override {
    std::vector<std::string> nomes;
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) nomes.push_back(std::string(dir.fileName().c_str()));
    return nomes;
  }
  bool gravar(const std::string &nome, const std::string &conteudo) override {
    File f = LittleFS.open(caminho(nome), "w");
    if (!f) return false;
    size_t escritos = f.write((const uint8_t *) conteudo.data(), conteudo.size());
    f.close();
    return escritos == conteudo.size();
  }
  bool ler(const std::string &nome, std::string &conteudo) override {
    File f = LittleFS.open(caminho(nome), "r");
    if (!f) return false;
    conteudo = std::string(f.readString().c_str());
    f.close();
    return true;
  }
  bool apagar(const std::string &nome) override {
    return LittleFS.remove(caminho(nome));
  }
  bool renomear(const std::string &de, const std::string &para) override {
    if (LittleFS.exists(caminho(para))) LittleFS.remove(caminho(para));
    return LittleFS.rename(caminho(de), caminho(para));
  }
private:
  static String caminho(const std::string &nome) { return String("/") + nome.c_str(); }
};

FlashLittleFS flashConfig;
TransacaoConfig transacaoConfig(flashConfig);

Task tarefaChecagem(5000, TASK_FOREVER, &checarAgendamentos);
Task tarefaPedirHora(30000, TASK_FOREVER, &pedirHoraSeNecessario);

// =======================================================================
// Função auxiliar para imprimir dados da flash na Serial.
// Só compila com -D DEBUG_FLASH: despejar todos os códigos na serial a cada
// Add_Code bloqueava o nó por vários segundos e travava o mesh.update().
// =======================================================================
#ifdef DEBUG_FLASH
void imprimirArquivosFlash() {
  Serial.println("\n==================================================");
  Serial.println("[LITTLEFS] LISTANDO ARQUIVOS SALVOS NA FLASH");
  Serial.println("==================================================");

  Dir dir = LittleFS.openDir("/"); //[cite: 2]
  int contadorDeArquivos = 0;

  while (dir.next()) { //[cite: 2]
    String nomeArquivo = dir.fileName(); //[cite: 2]
    File f = LittleFS.open("/" + nomeArquivo, "r"); //[cite: 2]
    if (f) {
      size_t tamanho = f.size();
      Serial.printf("-> Arquivo: '%s' (%d bytes)\n", nomeArquivo.c_str(), tamanho);
      Serial.print("   Conteudo: ");
      while (f.available()) {
        Serial.write(f.read());
      }
      Serial.println("\n--------------------------------------------------");
      f.close();
    }
    contadorDeArquivos++;
  }

  if (contadorDeArquivos == 0) {
    Serial.println("Nenhum arquivo encontrado na memoria Flash!");
  } else {
    Serial.printf("Total de %d arquivo(s) encontrado(s).\n", contadorDeArquivos);
  }
  Serial.println("==================================================\n");
}
#endif

// =======================================================================
// Envio de Status para a Central
// =======================================================================
void enviarStatusCentral() {
  JsonDocument docStatus;
  
  docStatus["command"] = "status_update"; 
  docStatus["id"] = myID;
  docStatus["status"] = powerStatus ? "Ligado" : "Desligado";
  docStatus["temp"] = temperaturaAlvo;

  String statusJSON;
  serializeJson(docStatus, statusJSON);

  mesh.sendBroadcast(statusJSON);
}

// =======================================================================
// Recepção e Processamento de Mensagens
// =======================================================================
void mensagensRecebidas(uint32_t nodeId_de_quem_enviou, String &msg) {
  JsonDocument filter;
  filter["command"]   = true;
  filter["id"]        = true;
  filter["new_id"]    = true;
  
  filter["timestamp"] = true; 
  filter["dia"]       = true;
  filter["hora"]      = true;
  filter["code"]      = true;
  filter["seq"]       = true;
  filter["hash"]      = true;   // Config_*
  filter["codigos"]   = true;   // Config_Inicio
  filter["agendas"]   = true;   // Config_Inicio

  JsonDocument docBasico;
  deserializeJson(docBasico, msg, DeserializationOption::Filter(filter));

  String command = docBasico["command"];

  // --- TRATAMENTO: REQUISIÇÃO DE ID ---
  if (command == "Require_IDs") {
    Serial.println("A Central pediu meu ID. Respondendo...");
    JsonDocument docResposta;
    docResposta["command"] = "Resposta_ID";
    docResposta["id"] = myID;
    String respostaJSON;
    serializeJson(docResposta, respostaJSON);
    mesh.sendSingle(nodeId_de_quem_enviou, respostaJSON);
  }
  
  // --- TRATAMENTO: MUDANÇA DE ID ---
  if (command == "Change_ID" && docBasico["id"] == myID) {
    String novoID = docBasico["new_id"].as<String>();

    if (novoID.length() > 0) {
        Serial.printf("Alterando ID de '%s' para '%s'\n", myID.c_str(), novoID.c_str());
        myID = novoID;

        File f = LittleFS.open("/nodeID.txt", "w"); //[cite: 2]
        if (f) {
            f.print(myID);
            f.close();
        }

        JsonDocument docResposta;
        docResposta["command"] = "Resposta_ID";
        docResposta["id"]      = myID;
        String respostaJSON;
        serializeJson(docResposta, respostaJSON);
        mesh.sendSingle(nodeId_de_quem_enviou, respostaJSON);
    }
  }

  // --- TRATAMENTO: SINCRONIZAÇÃO DE TEMPO ---
  if (command == "Sync_Time") {
    time_t epochTime = docBasico["timestamp"].as<long>(); 

    struct timeval tv;
    tv.tv_sec = epochTime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL); 

    Serial.println("Relógio interno atualizado com sucesso via Mesh!");
  }

  // --- TRATAMENTO: CONFIG COMPLETA E ATÔMICA (Config_*) ---
  if (command.startsWith("Config_") && docBasico["id"] == myID) {
    tratarConfig(command, docBasico, msg);
  }

  // --- TRATAMENTO: RECEBER AGENDAMENTO ---
  if (command == "Add_Schedule" && docBasico["id"] == myID) {
      int8_t diaNum = docBasico["dia"].as<int>(); 
      String horaStr = docBasico["hora"].as<String>();
      String codigo = docBasico["code"].as<String>();

      int separador = horaStr.indexOf(':');
      if (separador != -1) {
          int8_t h = horaStr.substring(0, separador).toInt();
          int8_t m = horaStr.substring(separador + 1).toInt();

          Agendamento novoAgendamento = {diaNum, h, m, codigo, false};
          listaAgendamentos.push_back(novoAgendamento);
          salvarAgendamento(novoAgendamento);

          Serial.printf("Agendamento salvo: Dia %d às %02d:%02d para código %s\n", 
                        diaNum, h, m, codigo.c_str());
      }
  }

  // --- TRATAMENTO: LIMPAR MEMÓRIA (Proteger nodeID.txt) ---
  if (command == "Clear_Memory" && docBasico["id"] == myID) {
      listaAgendamentos.clear(); 
      
      // CORREÇÃO: Remove todos os arquivos exceto nodeID.txt
      Dir dir = LittleFS.openDir("/"); //[cite: 2]
      while (dir.next()) { //[cite: 2]
          String nomeArquivo = dir.fileName(); //[cite: 2]
          if (nomeArquivo != "nodeID.txt") {
              LittleFS.remove("/" + nomeArquivo);
          }
      }
      Serial.println("Memória RAM e arquivos de códigos limpos. ID protegido.");

#ifdef DEBUG_FLASH
      imprimirArquivosFlash();
#endif
  }

  // --- TRATAMENTO: SALVAR CÓDIGO NOVO NA FLASH ---
  if (command == "Add_Code" && docBasico["id"] == myID) {
    DynamicJsonDocument docRaw(4096); // Alocação robusta para códigos longos[cite: 2]
    DeserializationError erro = deserializeJson(docRaw, msg);
      
    if (!erro) {
        if (docRaw.containsKey("name") && docRaw.containsKey("raw")) {
            String nome = docRaw["name"].as<String>();
            String raw = docRaw["raw"].as<String>();
              
            File f = LittleFS.open("/" + nome + ".txt", "w"); //[cite: 2]
            if (f) {
                f.print(raw);
                f.close();
                Serial.printf("Código IR '%s' salvo no arquivo /%s.txt\n", nome.c_str(), nome.c_str());

#ifdef DEBUG_FLASH
                imprimirArquivosFlash();
#endif
            } else {
                Serial.println("Erro ao criar arquivo na Flash!");
            }
        } else {
            Serial.println("Erro: JSON recebido não possui 'name' ou 'raw'.");
        }
    } else {
        Serial.print("Falha ao ler JSON gigante do Add_Code: ");
        Serial.println(erro.c_str()); 
    }
}

  // --- TRATAMENTO: DISPARO MANUAL ---
  if (command == "Dispatch" && docBasico["id"] == myID) {
      // TODO(tarefa 2): se a mensagem tiver "seq" e ele for igual a
      // ultimoSeqExecutado, é um reenvio de algo que já foi executado (o Ack
      // se perdeu). Nesse caso não disparar o IR de novo: só responder o Ack
      // outra vez e sair do bloco.

      JsonDocument docRaw;
      deserializeJson(docRaw, msg);

      if (docRaw.containsKey("raw")) {
          const char* rawStr = docRaw["raw"];
          Serial.printf("[DEBUG ESP] Recebido comando manual pela rede. Tamanho da string: %d caracteres.\n", strlen(rawStr));
          dispararStringRaw(rawStr);
      } 
      else if (docRaw.containsKey("name")) {
          String nomeComando = docRaw["name"].as<String>();
          Serial.printf("[DEBUG ESP] Disparo pela Flash solicitado. Arquivo: %s\n", nomeComando.c_str());
          dispararCodigoSalvo(nomeComando);
      } else {
          Serial.println("[DEBUG ESP] Comando 'Dispatch' recebido, mas não havia 'raw' nem 'name'.");
      }

      if (docRaw.containsKey("status")) powerStatus = (docRaw["status"] == "Ligado");
      if (docRaw.containsKey("temp")) temperaturaAlvo = docRaw["temp"].as<int>();
      
      enviarStatusCentral(); 

      // TODO(tarefa 2): confirmar a execução com
      // responderAck(nodeId_de_quem_enviou, seq) e guardar o seq em
      // ultimoSeqExecutado. Mensagens sem "seq" (formato antigo) não têm Ack.
  }

  // --- TRATAMENTO: PING (medição da malha) ---
  if (command == "Ping" && docBasico["id"] == myID) {
      // TODO(tarefa 3): responder {"command":"Pong","de":myID,"seq":<mesmo seq>}
      // com mesh.sendSingle(nodeId_de_quem_enviou, ...), sem fazer mais nada,
      // para que o tempo medido seja só o da rede.
  }
}

// =======================================================================
// Confirmação de entrega (tarefa 2)
// =======================================================================
// Deve mandar {"command":"Ack","de":myID,"seq":seq} só para "para" (a ponte).
void responderAck(uint32_t para, long seq) {
  // TODO(tarefa 2): montar o JSON acima e enviar com mesh.sendSingle(para, ...).
  (void) para;
  (void) seq;
}

// =======================================================================
// Config completa e atômica (pendência 5)
// =======================================================================
// Config_Inicio {hash, codigos, agendas} -> Config_Codigo {hash, nome, dados}
// -> Config_Agenda {hash, parte, itens: [[dia, "HH:MM", codigo], ...]}
// -> Config_Fim {hash}. Só no Config_Fim, com tudo recebido, a config nova
// substitui a antiga (veja transacao_config.h).
static const char *textoResultado(TransacaoConfig::Resultado r) {
  switch (r) {
    case TransacaoConfig::ACEITO:       return "aceito";
    case TransacaoConfig::IGNORADO:     return "ignorado";
    case TransacaoConfig::INVALIDO:     return "invalido";
    case TransacaoConfig::APLICADA:     return "aplicada";
    case TransacaoConfig::INCOMPLETA:   return "incompleta";
    case TransacaoConfig::ERRO_ARQUIVO: return "erro na flash";
  }
  return "?";
}

void tratarConfig(const String &command, JsonDocument &docBasico, String &msg) {
  std::string hash = docBasico["hash"] | "";
  TransacaoConfig::Resultado r = TransacaoConfig::IGNORADO;

  if (command == "Config_Inicio") {
    r = transacaoConfig.inicio(hash, docBasico["codigos"] | -1, docBasico["agendas"] | -1);
  }
  else if (command == "Config_Codigo") {
    // Mensagem inteira (o "dados" tem 1 a 3 KB e fica fora do filtro).
    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
      r = TransacaoConfig::INVALIDO;
    } else {
      std::string nome = doc["nome"] | "";
      std::string dados = doc["dados"] | "";
      r = transacaoConfig.codigo(hash, nome, dados);
      Serial.printf("[CONFIG] Codigo '%s': %s\n", nome.c_str(), textoResultado(r));
      return;
    }
  }
  else if (command == "Config_Agenda") {
    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
      r = TransacaoConfig::INVALIDO;
    } else {
      // Converte [[dia, "HH:MM", codigo], ...] para o formato do _agenda.cfg:
      // uma linha "dia,hora,minuto,codigo" por agendamento.
      std::string linhas;
      bool valido = true;
      for (JsonArray item : doc["itens"].as<JsonArray>()) {
        int dia = item[0] | -1;
        String hora = item[1] | "";
        std::string codigo = item[2] | "";
        int sep = hora.indexOf(':');
        int h = sep > 0 ? hora.substring(0, sep).toInt() : -1;
        int m = sep > 0 ? hora.substring(sep + 1).toInt() : -1;
        if (dia < 0 || dia > 6 || h < 0 || h > 23 || m < 0 || m > 59
            || !TransacaoConfig::nomeValido(codigo)) {
          valido = false;
          break;
        }
        char linha[48];
        snprintf(linha, sizeof(linha), "%d,%d,%d,%s\n", dia, h, m, codigo.c_str());
        linhas += linha;
      }
      r = valido ? transacaoConfig.agenda(hash, doc["parte"] | 0, linhas)
                 : TransacaoConfig::INVALIDO;
    }
  }
  else if (command == "Config_Fim") {
    r = transacaoConfig.fim(hash);
    if (r == TransacaoConfig::APLICADA) {
      carregarAgendamentos();   // a agenda nova passa a valer agora
    }
    if (r == TransacaoConfig::INCOMPLETA || r == TransacaoConfig::ERRO_ARQUIVO) {
      Serial.printf("[CONFIG] Config %s nao aplicada: %s\n", hash.c_str(), transacaoConfig.ultimoMotivo().c_str());
    }
  }

  Serial.printf("[CONFIG] %s (%s): %s\n", command.c_str(), hash.c_str(), textoResultado(r));
}

void dispararStringRaw(const char* rawStr) { 
  int count = 1;
  for (int i = 0; rawStr[i] != '\0'; i++) {
    if (rawStr[i] == ',') count++;
  }

  Serial.printf("[DEBUG ESP] Preparando para atirar... Array IR com %d posições.\n", count);

  uint16_t* pRaw = new uint16_t[count];
  
  if (pRaw != nullptr) {
    int index = 0;
    int currentVal = 0;
    
    for (int i = 0; rawStr[i] != '\0'; i++) {
      if (rawStr[i] == ',') {
        pRaw[index++] = currentVal;
        currentVal = 0; 
      } else if (isDigit(rawStr[i])) {
        currentVal = (currentVal * 10) + (rawStr[i] - '0');
      }
    }
    pRaw[index] = currentVal; 

    irsend.sendRaw(pRaw, count, 38);
    yield();
    delete[] pRaw;
    
    Serial.println("[DEBUG ESP] POU! Disparo IR executado com sucesso!");
  } else {
    Serial.println("[DEBUG ESP] ERRO CRÍTICO: Sem heap para o array IR!");
  }
}

void dispararCodigoSalvo(String nomeCodigo) {
  String caminhoArquivo = "/" + nomeCodigo + ".txt";
  File f = LittleFS.open(caminhoArquivo, "r");

  if (!f) {
    Serial.printf("[DEBUG ESP] Erro: Arquivo '%s' não encontrado na Flash!\n", caminhoArquivo.c_str());
    return;
  }

  Serial.printf("[DEBUG ESP] Iniciando disparo do agendamento: %s\n", caminhoArquivo.c_str());

  while (f.available()) {
    String linhaRaw = f.readStringUntil('\n');
    linhaRaw.trim(); 
    
    if (linhaRaw.length() > 0) {
      dispararStringRaw(linhaRaw.c_str());
      delay(250); 
    }
  }
  
  f.close();
  Serial.println("[DEBUG ESP] Sequência de disparo do agendamento finalizada!");
}

// =======================================================================
// Relógio e persistência dos agendamentos
// =======================================================================
bool relogioValido() {
  time_t agora = time(nullptr);
  struct tm infoTempo;
  localtime_r(&agora, &infoTempo);
  return infoTempo.tm_year + 1900 >= 2000;
}

// Depois de um reinício o relógio volta para 1970: pede a hora à central
// (via ponte) a cada 30 s até receber um Sync_Time.
void pedirHoraSeNecessario() {
  if (relogioValido()) return;

  JsonDocument docPedido;
  docPedido["command"] = "Need_Time";
  docPedido["id"] = myID;

  String pedidoJSON;
  serializeJson(docPedido, pedidoJSON);
  mesh.sendBroadcast(pedidoJSON);

  Serial.println("Relógio sem hora válida. Pedindo Sync_Time à central...");
}

// Uma linha por agendamento: "dia,hora,minuto,codigo". O arquivo é apagado
// pelo Clear_Memory junto com os códigos, então a sincronização o recria.
void salvarAgendamento(const Agendamento &agenda) {
  File f = LittleFS.open(ARQUIVO_AGENDAMENTOS, "a");
  if (!f) {
    Serial.println("Erro ao salvar agendamento na Flash!");
    return;
  }
  f.printf("%d,%d,%d,%s\n", agenda.diaDaSemana, agenda.hora, agenda.minuto, agenda.nomeCodigo.c_str());
  f.close();
}

void carregarAgendamentos() {
  listaAgendamentos.clear();

  File f = LittleFS.open(ARQUIVO_AGENDAMENTOS, "r");
  if (!f) return;

  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    int c1 = linha.indexOf(',');
    int c2 = linha.indexOf(',', c1 + 1);
    int c3 = linha.indexOf(',', c2 + 1);
    if (c1 == -1 || c2 == -1 || c3 == -1) continue;

    Agendamento agenda = {
      (int8_t) linha.substring(0, c1).toInt(),
      (int8_t) linha.substring(c1 + 1, c2).toInt(),
      (int8_t) linha.substring(c2 + 1, c3).toInt(),
      linha.substring(c3 + 1),
      false
    };
    listaAgendamentos.push_back(agenda);
  }
  f.close();

  Serial.printf("%d agendamento(s) carregado(s) da Flash.\n", listaAgendamentos.size());
}

void checarAgendamentos() {
    time_t agora;
    struct tm infoTempo;
    time(&agora);
    localtime_r(&agora, &infoTempo);

    if (infoTempo.tm_year + 1900 < 2000) {
        return; 
    }

    int8_t horaAtual = infoTempo.tm_hour;
    int8_t minutoAtual = infoTempo.tm_min;
    int8_t diaAtual = infoTempo.tm_wday; 

    for (auto &agenda : listaAgendamentos) {
        if (agenda.diaDaSemana == diaAtual && agenda.hora == horaAtual && agenda.minuto == minutoAtual) {
            if (!agenda.jaDisparou) {
                Serial.printf("Hora de disparar! Código: %s\n", agenda.nomeCodigo.c_str());
                dispararCodigoSalvo(agenda.nomeCodigo); 
                agenda.jaDisparou = true; 
            }
        } else {
            agenda.jaDisparou = false; 
        }
    }
}

// =======================================================================
// Ciclo de Vida do Microcontrolador
// =======================================================================
void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin()) {
      Serial.println("Formatando a memória Flash pela primeira vez...");
      LittleFS.format();
      LittleFS.begin();
  }

#ifdef DEBUG_FLASH
  imprimirArquivosFlash();
#endif

  // Rascunho de uma config que não chegou ao fim (placa desligada no meio).
  transacaoConfig.limparRascunhos();

  // O nó não tem RTC com bateria: a hora chega como epoch UTC pela malha e
  // precisa ser convertida para o horário de Brasília (UTC-3, sem horário de verão).
  setenv("TZ", FUSO_HORARIO, 1);
  tzset();

  carregarAgendamentos();

  if (LittleFS.exists("/nodeID.txt")) { //[cite: 2]
      File f = LittleFS.open("/nodeID.txt", "r"); //[cite: 2]
      myID = f.readString(); //[cite: 2]
      f.close(); //[cite: 2]
  } else {
      // ID padrão único por placa: com um ID fixo ("CI000"), duas placas novas
      // tinham o mesmo nome e um Change_ID renomeava as duas.
      myID = "CI-" + String(ESP.getChipId(), HEX);
      myID.toUpperCase();
  }

  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.setDebugMsgTypes(0);
  irsend.begin();
  mesh.onReceive(&mensagensRecebidas);

  userScheduler.addTask(tarefaChecagem);
  tarefaChecagem.enable();

  userScheduler.addTask(tarefaPedirHora);
  tarefaPedirHora.enable();
}

void loop() {
  mesh.update();
}