#!/bin/bash
# GERA O FIRMWARE PARA O M5BURNER - um .bin unico, gravado a partir de 0x0.
#
#   tools/m5burner.sh        P4 v1.x (os Tab5 vendidos ate aqui)
#   tools/m5burner.sh v3     P4 v3.x
#
# Compila DESTA pasta (todos os apps, menos os de SEM_APPS abaixo) e junta
# bootloader, tabela de particoes e app num arquivo so:
#   M5tabOS-<versao>-p4<chip>.bin  na raiz do projeto (fora do git).
#
# Gravar este arquivo APAGA os ajustes salvos (o espaco entre as partes e
# preenchido, inclusive a NVS). O firmware do C6 (ESP-NOW, Bluetooth) nao vai
# junto - o M5Burner nao grava o C6.
# Apps que NAO vao para o M5Burner (nome da pasta sem o app_, separados por
# espaco). Continuam na pasta e no ./flash.sh - so ficam fora deste arquivo.
SEM_APPS="cobra"

set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
source ./environment.sh

CHIP="${1:-v1}"
# Pasta de build propria: a do dia a dia (build/) continua com todos os apps.
SEM="$(echo $SEM_APPS | tr ' ' ';')"
case "$CHIP" in
    v1) PASTA=build_m5burner;    idf.py -B "$PASTA" -DM5_SEM_APPS="$SEM" build ;;
    v3) PASTA=build_m5burner_v3; idf.py -B "$PASTA" -DM5_CHIP=v3 -DM5_SEM_APPS="$SEM" build ;;
    *)  echo "uso: tools/m5burner.sh [v1|v3]"; exit 1 ;;
esac

VERSAO="$(head -1 version.txt | tr -d '[:space:]')"
SAIDA="$PWD/M5tabOS-$VERSAO-p4$CHIP.bin"
( cd "$PASTA" && esptool.py --chip esp32p4 merge_bin -o "$SAIDA" @flash_args > /dev/null )
echo ">>> $SAIDA ($(du -h "$SAIDA" | cut -f1)) - no M5Burner, gravar a partir de 0x0"
