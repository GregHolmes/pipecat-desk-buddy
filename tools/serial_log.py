"""Reset the CoreS3 and print its serial log for N seconds (non-interactive idf.py monitor).

Usage: python tools/serial_log.py [seconds] [--no-reset]
"""

import glob
import sys
import time

import serial

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("CoreS3 not found on USB (no /dev/cu.usbmodem*)")
PORT = ports[0]
seconds = float(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1][0].isdigit() else 30
reset = "--no-reset" not in sys.argv

with serial.Serial(PORT, 115200, timeout=0.2) as s:
    if reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
        *lines, buf = buf.split(b"\n")
        for line in lines:
            print(line.decode(errors="replace").rstrip(), flush=True)
    if buf:
        print(buf.decode(errors="replace").rstrip())
