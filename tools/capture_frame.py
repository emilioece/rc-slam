"""Capture JPEG frames from the FNK0085 ESP32-S3 camera firmware over serial.

    python tools/capture_frame.py --port COM5 --baud 921600 --out captures --count 3

Talks to firmware/esp32s3_cam: the board boots at 115200, the script switches
it to --baud with the BAUD command, then sends INFO and CAPTURE. Each frame
arrives as "FRAME <seq> <w> <h> JPEG <len> <crc32>\n", <len> raw bytes, "END\n".
DTR and RTS are held deasserted so opening the port does not reset the board.
"""
import argparse
import datetime
import pathlib
import sys
import time
import zlib

import serial

BOOT_BAUD = 115200


class CaptureError(Exception):
    pass


def open_port(port, baud):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as exc:
        text = str(exc)
        if "PermissionError" in text or "Access is denied" in text:
            raise CaptureError(f"{port} is busy; close any monitor, esptool, or PlatformIO upload using it")
        if "FileNotFoundError" in text or "could not open port" in text:
            raise CaptureError(f"{port} not found; check the cable and Device Manager ({text})")
        raise CaptureError(f"cannot open {port}: {text}")
    return ser


def drain(ser, seconds=0.15):
    """Drop leftover bytes so a retry does not treat payload as a new reply."""
    ser.reset_input_buffer()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if not chunk:
            time.sleep(0.01)


def read_line(ser, deadline):
    """Return the next non-empty protocol line, skipping log and boot noise."""
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = ser.read_until(b"\n")
        buf += chunk
        if not buf.endswith(b"\n"):
            continue
        line = buf.decode("ascii", errors="replace").strip()
        buf.clear()
        if line:
            return line
    raise CaptureError("timed out waiting for a reply")


def command(ser, cmd, expect, timeout):
    """Send cmd and return the first reply line starting with expect or ERR."""
    ser.write(cmd.encode("ascii") + b"\n")
    deadline = time.monotonic() + timeout
    while True:
        line = read_line(ser, deadline)
        if line.startswith(expect):
            return line
        if line.startswith("ERR "):
            raise CaptureError(f"{cmd}: {line}")
        print(f"  board: {line}")


def ping(ser, timeout):
    # The leading newline flushes any half line the board received at a wrong baud.
    ser.reset_input_buffer()
    ser.write(b"\nPING\n")
    deadline = time.monotonic() + timeout
    try:
        while read_line(ser, deadline) != "PONG":
            pass
    except CaptureError:
        return False
    return True


def connect(port, baud, timeout):
    """Open the port and leave the board talking at baud, wherever it started."""
    ser = open_port(port, baud)
    if ping(ser, 1.0):
        return ser
    if baud != BOOT_BAUD:
        ser.baudrate = BOOT_BAUD
        if not ping(ser, timeout):
            raise CaptureError(f"no PONG at {baud} or {BOOT_BAUD}; is firmware/esp32s3_cam flashed?")
        command(ser, f"BAUD {baud}", "OK BAUD", timeout)
        time.sleep(0.05)
        ser.baudrate = baud
        if not ping(ser, timeout):
            raise CaptureError(f"board stopped answering after switching to {baud}")
        return ser
    raise CaptureError(f"no PONG at {baud}; is firmware/esp32s3_cam flashed?")


def read_frame_once(ser, timeout):
    header = command(ser, "CAPTURE", "FRAME ", timeout).split()
    if len(header) != 7 or header[4] != "JPEG":
        raise CaptureError(f"malformed header: {' '.join(header)}")
    seq, width, height, length = (int(v) for v in (header[1], header[2], header[3], header[5]))
    crc = int(header[6], 16)

    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < length:
        if time.monotonic() > deadline:
            raise CaptureError(f"timeout: got {len(data)} of {length} payload bytes")
        data += ser.read(length - len(data))

    trailer = read_line(ser, time.monotonic() + timeout)
    if trailer != "END":
        raise CaptureError(f"expected END after payload, got {trailer!r}")
    actual = zlib.crc32(data)
    if actual != crc:
        raise CaptureError(f"CRC mismatch on frame {seq}: header {crc:08x}, payload {actual:08x}")
    if data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise CaptureError(f"frame {seq} is not a complete JPEG (SOI/EOI markers missing)")
    return seq, width, height, bytes(data)


def read_frame(ser, timeout):
    """Capture one frame; retry once after draining if the first attempt is corrupt."""
    try:
        return read_frame_once(ser, timeout)
    except CaptureError as first:
        print(f"  retry after: {first}")
        drain(ser)
        return read_frame_once(ser, timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("captures"))
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=5.0, help="seconds per reply")
    args = ap.parse_args()

    try:
        ser = connect(args.port, args.baud, args.timeout)
    except CaptureError as exc:
        sys.exit(f"error: {exc}")

    try:
        print(command(ser, "INFO", "INFO ", args.timeout))
        args.out.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        start = time.monotonic()
        for i in range(args.count):
            t0 = time.monotonic()
            seq, width, height, data = read_frame(ser, args.timeout)
            dt = time.monotonic() - t0
            path = args.out / f"frame_{stamp}_{seq:04d}.jpg"
            path.write_bytes(data)
            print(f"frame {seq}: {width}x{height} {len(data)} bytes, {dt * 1000:.0f} ms "
                  f"({1 / dt:.1f} fps) -> {path}")
        total = time.monotonic() - start
        print(f"{args.count} frames in {total:.2f} s, {args.count / total:.2f} fps average")
    except CaptureError as exc:
        sys.exit(f"error: {exc}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
