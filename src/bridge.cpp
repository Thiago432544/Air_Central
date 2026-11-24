#include "bridge.h"

// Inicialização dos objetos globais
painlessMesh mesh;
IRrecv irrecv(IR_RECV_PIN, CAPTURE_BUFFER_SIZE, TIMEOUT, true);
decode_results results;
bool modoLeituraIR = false;

// Tabela de rotas (tarefa 1): nome da sala ("id" das mensagens) -> nodeId da
// placa na malha. Começa vazia; enquanto um destino não estiver aqui, a ponte
// manda por broadcast, como sempre fez.
std::map<String, uint32_t> tabelaRotas;

// =======================================================================
// MESH -> QT (Via Cabo)
// =======================================================================
void mensagemRecebidaDaMesh(uint32_t from, String &msg) {
  // Tudo o que vem da malha continua indo para a central, sem filtro.
  Serial.println(msg);

  // TODO(tarefa 1): quando a mensagem for um "Resposta_ID", guardar em
  // tabelaRotas o par (id da mensagem -> from). O "from" é o nodeId de quem
  // mandou, que é o endereço que o sendSingle precisa.
}

// =======================================================================
// Envio para a malha: ponto único de saída das mensagens da central
// =======================================================================
// "destino" é o campo "id" da mensagem (a sala de destino). Vazio = broadcast.
void encaminharParaMalha(const String &mensagem, const String &destino) {
  // TODO(tarefa 1): se o destino estiver em tabelaRotas, mandar só para ele
  // com mesh.sendSingle(nodeId, mensagem) em vez de inundar a malha. Se não
  // estiver (ou se o envio falhar), cair no broadcast abaixo.
  (void) destino;
  mesh.sendBroadcast(mensagem);
}

// =======================================================================
// Topologia da malha (usada pela tarefa 3)
// =======================================================================
// Imprime numa linha só: {"command":"Topologia","t":<millis>,"nos":N,"arvore":{...}}
// "nos" = quantos nós a ponte enxerga (sem contar ela mesma) e "arvore" = a
// árvore de conexões que a própria painlessMesh monta.
void imprimirTopologia() {
  Serial.print("{\"command\":\"Topologia\",\"t\":");
  Serial.print(millis());
  Serial.print(",\"nos\":");
  Serial.print(mesh.getNodeList().size());
  Serial.print(",\"arvore\":");
  Serial.print(mesh.subConnectionJson());
  Serial.println("}");
}

// =======================================================================
// QT -> MESH ou BRIDGE
// =======================================================================
// Leitura não bloqueante: readStringUntil() esperava até 1 s quando a linha
// chegava incompleta, e nesse tempo o mesh.update() não rodava.
String bufferSerial;

void mensagemRecebidaDoQt() {
  while (Serial.available()) {
    char c = (char) Serial.read();

    if (c != '\n') {
      if (bufferSerial.length() < TAMANHO_MAX_LINHA) {
        bufferSerial += c;
      }
      continue;
    }

    String mensagemQt = bufferSerial;
    bufferSerial = "";
    mensagemQt.trim();

    if (mensagemQt.length() > 0) {
      // CORREÇÃO: Filtro estático para decodificar apenas "command" e "id"
      // Evita carregar o array "raw" gigante na memória RAM da Bridge!
      StaticJsonDocument<256> filter;
      filter["command"] = true;
      filter["id"] = true;

      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, mensagemQt, DeserializationOption::Filter(filter));
      
      if (!error) {
        String command = doc["command"].as<String>();
        if (command == "Require_IR") {
          modoLeituraIR = true;
          irrecv.enableIRIn();
          Serial.println("{\"log\":\"Aguardando sinal IR...\"}");
        }
        else if (command == "Topologia") {
          // Pedido da central para a própria ponte: não vai para a malha.
          imprimirTopologia();
        }
        else {
          // Repassa o JSON completo intacto para a rede Mesh
          encaminharParaMalha(mensagemQt, doc["id"] | "");
        }
      } else {
        Serial.printf("{\"log\":\"Erro decodificacao Bridge: %s\"}\n", error.c_str());
      }
    }
  }
}

// =======================================================================
// Leitura do Infravermelho (APENAS RAW)
// =======================================================================
void checarReceptorIR() {
  if (modoLeituraIR) {
    if (irrecv.decode(&results)) {
      
      String rawString = "";
      uint16_t count = results.rawlen - 1;

      for (uint16_t i = 1; i <= count; i++) {
        rawString += String(results.rawbuf[i] * RAWTICK);
        if (i < count) rawString += ",";
        yield(); // Evita crash no ESP8266 durante loops longos
      }

      DynamicJsonDocument docResposta(2048); 
      docResposta["command"] = "new_code";
      docResposta["type"] = "RAW";
      docResposta["code"] = rawString;
      docResposta["size"] = count;

      String respostaJSON;
      serializeJson(docResposta, respostaJSON);
      
      Serial.println(respostaJSON);

      modoLeituraIR = false;
      irrecv.disableIRIn();
      irrecv.resume();
    }
  }
}

// =======================================================================
// Ciclo de Vida do Microcontrolador
// =======================================================================
void setup() {
  // CORREÇÃO: Buffer serial aumentado para receber strings longas do Qt
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(1000);
  
  mesh.setDebugMsgTypes(0);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&mensagemRecebidaDaMesh);

  // Eventos da malha, impressos como linhas {"log":...} para a central e
  // para tools/console_serial.py. Servem para medir a reorganização (tarefa 3).
  mesh.onNewConnection([](uint32_t nodeId) {
    Serial.printf("{\"log\":\"nova_conexao\",\"t\":%lu,\"no\":%u}\n", millis(), nodeId);
  });
  mesh.onDroppedConnection([](uint32_t nodeId) {
    Serial.printf("{\"log\":\"conexao_perdida\",\"t\":%lu,\"no\":%u}\n", millis(), nodeId);
  });
  mesh.onChangedConnections([]() {
    Serial.printf("{\"log\":\"topologia_mudou\",\"t\":%lu,\"nos\":%u}\n", millis(), mesh.getNodeList().size());
  });
}

void loop() {
  mesh.update();            
  mensagemRecebidaDoQt();    
  checarReceptorIR();        
}