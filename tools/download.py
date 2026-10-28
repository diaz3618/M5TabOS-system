#!/usr/bin/env python3
# Copia um arquivo do cartao do Tab5 para o computador, pela mesma serial do monitor.
#
#   ./download.py /sdcard/apps/Sistema/energia.csv
#   ./download.py                  <- sem argumento, baixa o energia.csv
#
# Do outro lado quem responde e o comando "despejar" do console (console.cpp).
# Ele cala o ESP_LOG enquanto despeja e cerca o conteudo com >>>INICIO e >>>FIM,
# que e o que permite saber onde comeca, onde termina e se chegou tudo.
#
# FECHE O MONITOR ANTES. A porta e de um processo so: com o idf.py monitor
# aberto, este script nao consegue abrir a porta - ou pior, os dois leem
# metade cada um.

import sys, glob, time, os

# O pyserial nao esta no python do sistema - ele mora no venv do IDF. Em vez de
# exigir "source environment.sh" antes (passo facil de esquecer), o proprio arquivo se reexecuta com o interpretador
# certo na primeira falha de import. Roda direto, clicando ou pelo terminal.
try:
    import serial
except ImportError:
    # O python do ambiente do ESP-IDF (IDF_PYTHON_ENV_PATH, definido pelo
    # export.sh / environment.sh).
    IDF_PY = os.path.join(os.environ.get("IDF_PYTHON_ENV_PATH", ""), "bin", "python")
    if os.path.exists(IDF_PY) and os.environ.get("_BAIXAR_REEXEC") != "1":
        os.environ["_BAIXAR_REEXEC"] = "1"
        os.execv(IDF_PY, [IDF_PY, os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit("Falta o pyserial. Rode:  source environment.sh && ./download.py")

ORIGEM  = sys.argv[1] if len(sys.argv) > 1 else "/sdcard/apps/Sistema/energia.csv"
DESTINO = ORIGEM.split("/")[-1]

# DOIS PRAZOS, e a diferenca importa.
#
# Antes de o despejo comecar, o unico motivo de silencio e alguma coisa errada -
# porta ocupada pelo monitor, firmware sem o comando, cartao fora. Esperar dois
# minutos por isso e so demorar a dar a ma noticia.
#
# Depois que ele comecou, um silencio longo e normal: o Tab5 pausa a cada 64
# linhas para nao segurar a CPU, e o cartao as vezes trava um instante.
ESPERA_INICIO = 8       # ate a primeira resposta
ESPERA_FLUXO  = 60      # entre uma linha e a proxima, ja despejando

portas = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
if not portas:
    sys.exit("Nenhuma porta USB do Tab5. Ele esta ligado e conectado?")
porta = portas[0]

print(f">>> {porta}: pedindo {ORIGEM}")
s = serial.Serial(porta, 115200, timeout=1)
time.sleep(0.3)
s.reset_input_buffer()
s.write(f"despejar {ORIGEM}\n".encode())

linhas   = []
dentro   = False
esperado = None
contadas = None
mudo_ate = time.time() + ESPERA_INICIO

while time.time() < mudo_ate:
    bruto = s.readline()
    if not bruto:
        continue
    mudo_ate = time.time() + (ESPERA_FLUXO if dentro else ESPERA_INICIO)
    linha = bruto.decode("utf-8", "replace").rstrip("\r\n")

    # Firmware sem o comando. Sem este teste o script ficava esperando o
    # >>>INICIO que nunca vem, e o sintoma era "travou" em vez de "grave o
    # firmware novo" - a mesma informacao, entregue tarde demais para servir.
    if "comando desconhecido" in linha:
        sys.exit("Este Tab5 ainda nao tem o comando 'despejar'.\n"
                 "Grave o firmware novo:  ./flash.sh")

    if linha.startswith(">>>INICIO"):
        dentro = True
        partes = linha.split()
        if len(partes) >= 3:
            esperado = int(partes[2])
            print(f"    {esperado} bytes a caminho...")
        continue

    if linha.startswith(">>>FIM"):
        contadas = linha.split()[1]
        break

    if linha.startswith("erro:"):
        sys.exit(f"O Tab5 respondeu: {linha}")

    if dentro:
        linhas.append(linha)
        if len(linhas) % 200 == 0:
            print(f"    {len(linhas)} linhas", end="\r", flush=True)

s.close()

if contadas is None:
    if not dentro:
        sys.exit("O Tab5 nao respondeu.\n"
                 "  - o monitor serial esta aberto em outra janela? "
                 "a porta e de um processo so\n"
                 "  - o cartao esta no aparelho?")
    sys.exit(f"\nO despejo parou no meio ({len(linhas)} linhas). "
             "Rode de novo - arquivo pela metade nao serve para ajustar curva.")

corpo = "\n".join(linhas) + "\n"
open(DESTINO, "w").write(corpo)

# CONFERE O TAMANHO, e nao so a contagem de linhas. Byte perdido na serial nao
# apaga uma linha inteira - ele corrompe um numero no meio, e uma curva ajustada
# em cima de numero corrompido mente sem dar sinal.
recebido = len(corpo.encode())
print(f"\n>>> {DESTINO}: {len(linhas)} linhas, {recebido} bytes")
if esperado is not None and recebido != esperado:
    print(f"!!! o Tab5 disse {esperado} bytes e chegaram {recebido}.")
    print("    Provavel byte perdido - rode de novo antes de usar este arquivo.")
