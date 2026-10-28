#!/bin/bash
# Builds and flashes M5tabOS onto the Tab5, then opens the serial monitor.
# Usage:  ./flash.sh          (Ctrl+] exits the monitor)
cd "$(dirname "$0")" || exit 1
source ./environment.sh

# The Tab5's port (P4's native USB-Serial-JTAG): /dev/cu.usbmodem* on macOS,
# /dev/ttyACM* on Linux. PORT=... ./flash.sh picks a different one.
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)
fi
if [ -z "$PORT" ]; then
    echo "No Tab5 USB port found. Is the Tab5 powered on and connected?"
    exit 1
fi

# THE P4 REVISION decides the binary (see sdkconfig.defaults.chip_v1/_v3): a
# v1.x binary does not run on a v3.x chip, nor the other way around. Read
# from the device itself; CHIP=v1 or CHIP=v3 ./flash.sh skips the probe.
if [ -z "$CHIP" ]; then
    REV=$(esptool.py --chip esp32p4 -p "$PORT" chip-id 2>/dev/null | grep -o "revision v[0-9]*" | head -1)
    case "$REV" in
        "revision v3"*) CHIP=v3 ;;
        "revision v"*)  CHIP=v1 ;;
        *) echo "Could not read the chip revision - using v1 (CHIP=v3 ./flash.sh forces the other)"; CHIP=v1 ;;
    esac
fi
echo ">>> P4 $CHIP, flashing on $PORT"

if [ "$CHIP" = "v3" ]; then
    idf.py -B build_v3 -DM5_CHIP=v3 -p "$PORT" flash monitor
else
    idf.py -p "$PORT" flash monitor
fi
