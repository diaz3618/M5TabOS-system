#!/bin/bash
# Builds and flashes the C6 firmware through the serial programmer (e.g. an
# M5Stack ESP32 Downloader), with the Tab5 POWERED OFF - the programmer
# powers the C6.
# Usage:  c6/flash.sh
cd "$(dirname "$0")" || exit 1
source ../environment.sh

# The programmer's port (CH9102 USB-serial bridge): /dev/cu.usbserial-* on
# macOS, /dev/ttyUSB* on Linux. Don't confuse it with the Tab5's
# (/dev/cu.usbmodem*), which is the P4. PORT=... c6/flash.sh picks a
# different one.
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null | head -1)
fi
if [ -z "$PORT" ]; then
    echo "No programmer found. Connected via USB-A? (USB-C to USB-C leaves it unpowered)"
    exit 1
fi
echo ">>> flashing the C6 on $PORT"

idf.py -p "$PORT" flash || exit 1
echo ">>> done. Power off the programmer, power on the Tab5, and run 'farejo info' in the console."
