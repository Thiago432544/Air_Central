"""Console da ponte do Air Central pela USB.

Uso:
    pip install pyserial
    python tools/console_serial.py COM5        (Windows)
    python tools/console_serial.py /dev/ttyUSB0 (Linux)

Imprime cada linha que a ponte manda, com o tempo em ms desde que o console
abriu, e envia para a ponte o que for digitado. Atalhos:

    /ids                 pede o ID de todos os nós (Require_IDs)
    /topologia           pede a árvore da malha à ponte
    /ping "SALA" [N]     manda N pings para a sala (padrão 10)
    /sair                fecha o console

Veja docs/REDE.md para o formato das mensagens.
"""

import json
import shlex
import sys
import threading
import time

import serial  # pyserial

VELOCIDADE = 115200

inicio = time.monotonic()

# seq -> instante (em segundos, time.monotonic) em que o Pong chegou.
pongs = {}
pongs_lock = threading.Lock()

proximo_seq = 1


def agora_ms():
    return int((time.monotonic() - inicio) * 1000)


def enviar(porta, objeto_ou_texto):
    texto = objeto_ou_texto
    if not isinstance(texto, str):
        texto = json.dumps(objeto_ou_texto, separators=(",", ":"))
    porta.write((texto + "\n").encode("utf-8"))
    print(f"[{agora_ms():>8} ms] >> {texto}")


def ao_receber(linha):
    """Chamada para cada linha que chega da ponte."""
    chegada = time.monotonic()
    try:
        msg = json.loads(linha)
    except ValueError:
        return
    if isinstance(msg, dict) and msg.get("command") == "Pong" and "seq" in msg:
        with pongs_lock:
            pongs[msg["seq"]] = chegada


def ler_serial(porta, parar):
    while not parar.is_set():
        try:
            bruto = porta.readline()
        except serial.SerialException as erro:
            print(f"Erro na serial: {erro}")
            parar.set()
            return
        if not bruto:
            continue
        linha = bruto.decode("utf-8", errors="replace").strip()
        if not linha:
            continue
        print(f"[{agora_ms():>8} ms] << {linha}")
        ao_receber(linha)


def medir_ping(porta, sala, n):
    """Manda n pings para a sala e mostra o RTT de cada um."""
    # TODO(tarefa 3): para cada um dos n pings:
    #   1. pegar um seq novo (proximo_seq) e anotar time.monotonic() no envio;
    #   2. enviar(porta, {"command": "Ping", "id": sala, "seq": seq});
    #   3. esperar o pongs[seq] aparecer (ao_receber já guarda a hora de
    #      chegada), com um tempo limite (ex.: 2 s) para contar como perdido;
    #   4. RTT = chegada - envio, em ms.
    # No fim, mostrar mínimo, média, máximo e quantos se perderam.
    print(f"medir_ping ainda não foi implementado (tarefa 3). Sala: {sala}, N: {n}")


def tratar_atalho(porta, linha):
    """Devolve False quando é para sair."""
    try:
        partes = shlex.split(linha)
    except ValueError as erro:
        print(f"Comando inválido: {erro}")
        return True

    comando = partes[0].lower()
    if comando == "/sair":
        return False
    if comando == "/ids":
        enviar(porta, {"command": "Require_IDs"})
    elif comando == "/topologia":
        enviar(porta, {"command": "Topologia"})
    elif comando == "/ping":
        if len(partes) < 2:
            print('Uso: /ping "SALA" [N]')
            return True
        n = 10
        if len(partes) >= 3:
            try:
                n = int(partes[2])
            except ValueError:
                print("N tem que ser um número inteiro.")
                return True
        medir_ping(porta, partes[1], n)
    else:
        print("Atalhos: /ids, /topologia, /ping \"SALA\" [N], /sair")
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    porta = serial.Serial(sys.argv[1], VELOCIDADE, timeout=0.2)
    print(f"Conectado em {sys.argv[1]} a {VELOCIDADE} baud. Digite /sair para fechar.")

    parar = threading.Event()
    leitor = threading.Thread(target=ler_serial, args=(porta, parar), daemon=True)
    leitor.start()

    try:
        while not parar.is_set():
            linha = input().strip()
            if not linha:
                continue
            if linha.startswith("/"):
                if not tratar_atalho(porta, linha):
                    break
            else:
                enviar(porta, linha)
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        parar.set()
        leitor.join(timeout=1)
        porta.close()


if __name__ == "__main__":
    main()
