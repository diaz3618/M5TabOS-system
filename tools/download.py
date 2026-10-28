#!/usr/bin/env python3
# Copy a file from the Tab5 card to the computer over the same serial connection used by the monitor.
#
#   ./download.py /sdcard/apps/Sistema/energia.csv
#   ./download.py                  <- without an argument, downloads energia.csv
#
# The console command "despejar" in console.cpp responds on the other end.
# It silences ESP_LOG while dumping the file and surrounds the contents with >>>INICIO and >>>FIM,
# which tells us where the transfer starts, where it ends, and whether it arrived intact.
#
# CLOSE THE MONITOR FIRST. Only one process can own the port: with `idf.py monitor`
# open, this script cannot open the port—or worse, both programs may read half of the data.

import sys, glob, time, os

# PySerial is not installed in the system Python; it lives in the IDF virtual environment. Instead of
# requiring `source environment.sh` first (an easy step to forget), this file re-executes itself with
# the correct interpreter after the first failed import. It works directly from a click or the terminal.
try:
    import serial
except ImportError:
    # The ESP-IDF environment's Python (IDF_PYTHON_ENV_PATH, set by
    # export.sh / environment.sh).
    IDF_PY = os.path.join(os.environ.get("IDF_PYTHON_ENV_PATH", ""), "bin", "python")
    if os.path.exists(IDF_PY) and os.environ.get("_BAIXAR_REEXEC") != "1":
        os.environ["_BAIXAR_REEXEC"] = "1"
        os.execv(IDF_PY, [IDF_PY, os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit("PySerial is missing. Run:  source environment.sh && ./download.py")

SOURCE_PATH = sys.argv[1] if len(sys.argv) > 1 else "/sdcard/apps/Sistema/energia.csv"
DESTINATION = SOURCE_PATH.split("/")[-1]

# TWO TIMEOUTS, and the distinction matters.
#
# Before the dump starts, silence means something is wrong—the monitor owns the port,
# the firmware lacks the command, or the card is missing. Waiting two minutes only
# delays delivering the bad news.
#
# After it starts, a long pause is normal: the Tab5 pauses every 64 lines so it
# does not monopolize the CPU, and the card sometimes stalls briefly.
START_TIMEOUT = 8       # until the first response
STREAM_TIMEOUT = 60     # between lines while dumping

ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
if not ports:
    sys.exit("No Tab5 USB port found. Is it powered on and connected?")
port = ports[0]

print(f">>> {port}: requesting {SOURCE_PATH}")
s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.3)
s.reset_input_buffer()
s.write(f"despejar {SOURCE_PATH}\n".encode())

lines = []
inside = False
expected_bytes = None
counted_lines = None
timeout_deadline = time.time() + START_TIMEOUT

while time.time() < timeout_deadline:
    raw_line = s.readline()
    if not raw_line:
        continue
    timeout_deadline = time.time() + (STREAM_TIMEOUT if inside else START_TIMEOUT)
    line = raw_line.decode("utf-8", "replace").rstrip("\r\n")

    # The firmware lacks the command. Without this test, the script would wait for
    # the >>>INICIO that never arrives, reporting "stalled" instead of "flash the
    # new firmware"—the same information delivered too late to be useful.
    if "comando desconhecido" in line:
        sys.exit("This Tab5 does not have the 'despejar' command yet.\n"
                 "Flash the new firmware:  ./flash.sh")

    if line.startswith(">>>INICIO"):
        inside = True
        fields = line.split()
        if len(fields) >= 3:
            expected_bytes = int(fields[2])
            print(f"    {expected_bytes} bytes incoming...")
        continue

    if line.startswith(">>>FIM"):
        counted_lines = line.split()[1]
        break

    if line.startswith("erro:"):
        sys.exit(f"The Tab5 replied: {line}")

    if inside:
        lines.append(line)
        if len(lines) % 200 == 0:
            print(f"    {len(lines)} lines", end="\r", flush=True)

s.close()

if counted_lines is None:
    if not inside:
        sys.exit("The Tab5 did not respond.\n"
                 "  - is the serial monitor open in another window? "
                 "only one process can own the port\n"
                 "  - is the card in the device?")
    sys.exit(f"\nThe dump stopped partway through ({len(lines)} lines). "
             "Run it again—a partial file is not suitable for fitting the curve.")

body = "\n".join(lines) + "\n"
open(DESTINATION, "w").write(body)

# CHECK THE SIZE, not just the line count. A byte lost over serial does not
# remove an entire line; it corrupts a number in the middle, and a curve fitted
# to a corrupt number can lie without giving any warning.
received_bytes = len(body.encode())
print(f"\n>>> {DESTINATION}: {len(lines)} lines, {received_bytes} bytes")
if expected_bytes is not None and received_bytes != expected_bytes:
    print(f"!!! The Tab5 reported {expected_bytes} bytes, but {received_bytes} arrived.")
    print("    A byte was probably lost—run it again before using this file.")
