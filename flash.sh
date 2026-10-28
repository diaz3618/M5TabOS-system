#!/bin/bash
# Compila e grava o M5tabOS no Tab5, e abre o monitor serial.
# Uso:  ./flash.sh          (Ctrl+] sai do monitor)
cd "$(dirname "$0")" || exit 1
source ./environment.sh

# A porta do Tab5 (USB-Serial-JTAG nativo do P4): /dev/cu.usbmodem* no macOS,
# /dev/ttyACM* no Linux. PORTA=... ./flash.sh escolhe outra.
if [ -z "$PORTA" ]; then
    PORTA=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)
fi
if [ -z "$PORTA" ]; then
    echo "Nenhuma porta USB do Tab5 encontrada. O Tab5 esta ligado e conectado?"
    exit 1
fi

# A REVISAO DO P4 decide o binario (ver sdkconfig.defaults.chip_v1/_v3): um
# binario de v1.x nao roda num v3.x, nem o contrario. Lida do proprio
# aparelho; CHIP=v1 ou CHIP=v3 ./flash.sh pula a leitura.
if [ -z "$CHIP" ]; then
    REV=$(esptool.py --chip esp32p4 -p "$PORTA" chip-id 2>/dev/null | grep -o "revision v[0-9]*" | head -1)
    case "$REV" in
        "revision v3"*) CHIP=v3 ;;
        "revision v"*)  CHIP=v1 ;;
        *) echo "Nao consegui ler a revisao do chip - usando v1 (CHIP=v3 ./flash.sh forca a outra)"; CHIP=v1 ;;
    esac
fi
echo ">>> P4 $CHIP, gravando em $PORTA"

if [ "$CHIP" = "v3" ]; then
    idf.py -B build_v3 -DM5_CHIP=v3 -p "$PORTA" flash monitor
else
    idf.py -p "$PORTA" flash monitor
fi
