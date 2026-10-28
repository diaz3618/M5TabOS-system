#!/bin/bash
# BUILDS THE FIRMWARE FOR M5BURNER - a single .bin, flashed starting at 0x0.
#
#   tools/m5burner.sh        P4 v1.x (the Tab5s sold so far)
#   tools/m5burner.sh v3     P4 v3.x
#
# Builds FROM THIS FOLDER (every app, except the ones in WITHOUT_APPS below)
# and merges the bootloader, partition table, and app into one file:
#   M5tabOS-<version>-p4<chip>.bin  at the project root (outside git).
#
# Flashing this file ERASES the saved settings (the space between the parts
# gets filled in, including NVS). The C6 firmware (ESP-NOW, Bluetooth) does
# not come with it - M5Burner does not flash the C6.
# Apps that do NOT go into the M5Burner build (folder name without the
# app_ prefix, space-separated). They stay in their folder and in
# ./flash.sh - they're only left out of this file.
WITHOUT_APPS="cobra"

set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
source ./environment.sh

CHIP="${1:-v1}"
# A build folder of its own: the everyday one (build/) keeps every app.
WITHOUT="$(echo $WITHOUT_APPS | tr ' ' ';')"
case "$CHIP" in
    v1) FOLDER=build_m5burner;    idf.py -B "$FOLDER" -DM5_SEM_APPS="$WITHOUT" build ;;
    v3) FOLDER=build_m5burner_v3; idf.py -B "$FOLDER" -DM5_CHIP=v3 -DM5_SEM_APPS="$WITHOUT" build ;;
    *)  echo "usage: tools/m5burner.sh [v1|v3]"; exit 1 ;;
esac

VERSION="$(head -1 version.txt | tr -d '[:space:]')"
OUTPUT="$PWD/M5tabOS-$VERSION-p4$CHIP.bin"
( cd "$FOLDER" && esptool.py --chip esp32p4 merge_bin -o "$OUTPUT" @flash_args > /dev/null )
echo ">>> $OUTPUT ($(du -h "$OUTPUT" | cut -f1)) - in M5Burner, flash starting at 0x0"
