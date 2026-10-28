#!/bin/bash
# Compila e grava o firmware do C6 pelo gravador serial (ex.: M5Stack ESP32
# Downloader), com o Tab5 DESLIGADO - o gravador alimenta o C6.
# Uso:  c6/flash.sh
cd "$(dirname "$0")" || exit 1
source ../environment.sh

# A porta do gravador (ponte USB-serial CH9102): /dev/cu.usbserial-* no macOS,
# /dev/ttyUSB* no Linux. Nao confundir com a do Tab5 (/dev/cu.usbmodem*), que e
# o P4. PORTA=... c6/flash.sh escolhe outra.
if [ -z "$PORTA" ]; then
    PORTA=$(ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null | head -1)
fi
if [ -z "$PORTA" ]; then
    echo "Nenhum gravador encontrado. Ligado por USB-A? (com USB-C para USB-C ele fica sem energia)"
    exit 1
fi
echo ">>> gravando o C6 em $PORTA"

idf.py -p "$PORTA" flash || exit 1
echo ">>> pronto. Desligue o gravador, ligue o Tab5 e rode 'farejo info' no console."
