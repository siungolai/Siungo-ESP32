"""Read the ESP32-S3 serial port directly (pio device monitor needs a TTY).

    python tools/serial-read.py [seconds] [--port COM4] [--baud 115200]

Writes raw bytes to .doc/serial-raw.txt with a byte-exact copy (no codec
translation) so a decode problem is visible as a decode problem. The port and
baud default to the project's measured facts: COM4 / 115200 (VID:PID
303A:1001), but they are flags rather than constants because the board moves
between USB ports and the next bench may not be COM4.
"""
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BAUD = 115200


def parse_args(argv):
    """seconds [--port COM] [--baud N], all optional and order-independent."""
    seconds = 30.0
    port = "COM4"
    baud = BAUD
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--port":
            port = argv[i + 1]
            i += 2
        elif arg == "--baud":
            baud = int(argv[i + 1])
            i += 2
        elif arg.startswith("--port="):
            port = arg.split("=", 1)[1]
            i += 1
        elif arg.startswith("--baud="):
            baud = int(arg.split("=", 1)[1])
            i += 1
        else:
            seconds = float(arg)
            i += 1
    return seconds, port, baud


def main():
    seconds, port, baud = parse_args(sys.argv[1:])
    try:
        import serial
    except ImportError:
        print("pyserial missing; nothing was read")
        return 2
    out = ROOT / ".doc" / "serial-raw.txt"
    out.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + seconds
    chunks = []
    with serial.Serial(port, baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        while time.time() < deadline:
            waiting = ser.in_waiting
            if waiting:
                chunks.append(ser.read(waiting))
    raw = b"".join(chunks)
    out.write_bytes(raw)
    print("read %d bytes -> %s" % (len(raw), out))
    text = raw.decode("utf-8", "replace")
    print("---- decoded ----")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
