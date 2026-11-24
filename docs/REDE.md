# Rede do Air Central — formato das mensagens (v1)

Caminho de uma mensagem:

```
Central (Qt) --USB serial 115200--> Ponte (ESP8266) --painlessMesh--> Nó da sala (ESP8266) --IR--> ar
```

Cada mensagem é **um JSON numa linha só**, terminado em `\n`. A ponte corta
linhas maiores que 4.096 bytes (`TAMANHO_MAX_LINHA` em `include/bridge.h`) e
o JSON cortado é rejeitado.

## Campos

| Campo     | Significado |
| --------- | ----------- |
| `command` | Tipo da mensagem (`Dispatch`, `Add_Code`, `Ack`, ...). Sempre presente. |
| `id`      | Central → nó: **sala de destino** (o "tópico"). Sem `id` = broadcast, todos recebem. Nas mensagens antigas nó → central (`Resposta_ID`, `status_update`, `Need_Time`), `id` é quem mandou. |
| `de`      | Quem mandou. Usado nas mensagens novas nó → central (`Ack`, `Pong`). |
| `seq`     | Inteiro crescente gerado pela central. O nó devolve o mesmo `seq` na resposta (`Ack` ou `Pong`), e é assim que a central sabe a qual envio a resposta se refere. |

O formato v1 é compatível com o atual: um nó antigo ignora os campos que não
conhece, e mensagens sem `seq` continuam funcionando (só não têm confirmação).

## Regra de envio

- Central → ponte: a ponte lê só `command` e `id` (filtro do ArduinoJson, para
  não carregar o `raw` gigante na RAM). `Require_IR` e `Topologia` são para a
  própria ponte. O resto vai para `encaminharParaMalha(mensagem, id)`.
- Ponte → malha: hoje é sempre `sendBroadcast`. Com a **tarefa 1**, quando o
  `id` estiver na `tabelaRotas`, vai com `sendSingle` só para aquele nó.
- Malha → ponte → central: a ponte imprime na serial tudo o que recebe da
  malha, sem filtro.
- Nó → central: as respostas novas (`Ack`, `Pong`) vão com `sendSingle` para
  quem mandou (a ponte), não por broadcast.

## Mensagens novas

| Mensagem | Direção | Exemplo |
| -------- | ------- | ------- |
| `Ack` | nó → central | `{"command":"Ack","de":"LAB-01","seq":42}` |
| `Ping` | central → nó | `{"command":"Ping","id":"LAB-01","seq":7}` |
| `Pong` | nó → central | `{"command":"Pong","de":"LAB-01","seq":7}` |
| `Topologia` | central → ponte | `{"command":"Topologia"}` |
| `Topologia` | ponte → central | `{"command":"Topologia","t":123456,"nos":5,"arvore":{...}}` |

Na resposta da `Topologia`, `t` é o `millis()` da ponte, `nos` é quantos nós
ela enxerga na malha (sem contar ela mesma) e `arvore` é o
`mesh.subConnectionJson()` da painlessMesh.

## Config completa da sala (`Config_*`)

Substitui o trio `Clear_Memory` + `Add_Code` + `Add_Schedule` na sincronização.
A central manda, para cada sala (todas com o `id` da sala e o mesmo `hash`):

| Mensagem | Exemplo |
| -------- | ------- |
| `Config_Inicio` | `{"command":"Config_Inicio","id":"LAB-01","hash":"3f2a9c1b","codigos":17,"agendas":1}` |
| `Config_Codigo` | `{"command":"Config_Codigo","id":"LAB-01","hash":"3f2a9c1b","nome":"22","dados":"9000,4500,..."}` |
| `Config_Agenda` | `{"command":"Config_Agenda","id":"LAB-01","hash":"3f2a9c1b","parte":1,"itens":[[1,"07:00","22"]]}` |
| `Config_Fim` | `{"command":"Config_Fim","id":"LAB-01","hash":"3f2a9c1b"}` |

O nó grava tudo como rascunho e só troca pela config atual no `Config_Fim`,
se chegou tudo o que o `Config_Inicio` anunciou. Se faltou algo, descarta o
rascunho e mantém a config antiga inteira. O `hash` fica gravado na placa
(`/_config.hash`). Mensagens repetidas não estragam nada. A lógica está em
`include/transacao_config.h`, com teste no PC em `test/host/`.

Esta parte não confirma nada para a central: confirmação e reenvio de cada
mensagem são da tarefa 2, e a central descobre quem está desatualizado pelo
hash (heartbeat, mais adiante).

## Eventos da ponte

A ponte imprime sozinha, sem ninguém pedir, uma linha para cada mudança na
malha (o `t` é o `millis()` da ponte):

```
{"log":"nova_conexao","t":120034,"no":2733512345}
{"log":"conexao_perdida","t":180770,"no":2733512345}
{"log":"topologia_mudou","t":180771,"nos":4}
```

A central só mostra essas linhas no `qDebug`. O `tools/console_serial.py`
mostra tudo com a hora de chegada, o que ajuda a medir quanto tempo a malha
leva para se reorganizar (tarefa 3).

## Onde estão os TODOs

| Tarefa | Arquivo | O que falta |
| ------ | ------- | ----------- |
| 1 — endereçamento e roteamento | `src/bridge.cpp` | Preencher `tabelaRotas` em `mensagemRecebidaDaMesh` com o `Resposta_ID`; usar `sendSingle` em `encaminharParaMalha`. |
| 2 — entrega confiável | `src/node.cpp` | No `Dispatch`: não repetir o IR se o `seq` for igual a `ultimoSeqExecutado`; confirmar com `responderAck`; escrever `responderAck`. |
| 2 — entrega confiável | central, `src/senddata.cpp` (branch `rede/base-central`) | Pendentes, timeout, reenvio e `deliveryFailed` em `send_reliable` e `on_ack`. |
| 3 — medição da malha | `src/node.cpp` | Responder `Pong` ao `Ping`. |
| 3 — medição da malha | `tools/console_serial.py` | `medir_ping`: mandar N pings e calcular o RTT de cada um. |

Procure por `TODO(tarefa N)` no código.

## Console serial

```
pip install pyserial
python tools/console_serial.py COM5
```

Atalhos: `/ids` (pede o ID de todos os nós), `/topologia`, `/ping "SALA" [N]`
e `/sair`. Qualquer outra linha digitada vai para a ponte do jeito que foi
escrita (tem que ser um JSON numa linha). Feche a central antes: só um
programa pode abrir a porta serial por vez.
