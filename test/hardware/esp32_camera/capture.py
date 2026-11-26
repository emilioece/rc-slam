"""Grab one JPEG from the camera test firmware and save it.

    python capture.py [--port COM5] [--out capture.jpg]
"""
import argparse
import os
import sys
import time

import serial
from serial.tools import list_ports

BAUD = 921600
CH343_VID = 0x1A86


def find_port():
    for p in list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    sys.exit("No CH343 serial port found; pass --port explicitly.")


def wait_ready(ser, timeout_s=20):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ser.write(b"p")
        line = ser.readline().decode(errors="replace").strip()
        if line:
            print(line)
        if line == "READY":
            return
    sys.exit("Board never replied READY. Is the test firmware flashed?")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default="capture.jpg")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port or find_port()
    ser.baudrate = BAUD
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = False
    ser.open()

    wait_ready(ser)
    ser.reset_input_buffer()
    ser.write(b"c")

    ser.timeout = 5
    header = ser.readline().decode(errors="replace").strip()
    if not header.startswith("JPEG "):
        sys.exit(f"Unexpected reply: {header!r}")
    size = int(header.split()[1])
    data = ser.read(size)
    if len(data) != size:
        sys.exit(f"Short read: got {len(data)} of {size} bytes")
    if data[:2] != b"\xff\xd8":
        sys.exit("Data is not a JPEG (missing FFD8 start marker)")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"Saved {size} bytes to {args.out}")
    ser.close()


if __name__ == "__main__":
    main()
