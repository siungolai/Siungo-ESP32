"""Timestamped serial capture with board reset (CH340/CP2102 DTR/RTS pulse).

    python tools/serial-log.py [seconds] [--port COM4] [--baud 115200]

pio device monitor needs an interactive stdin, so this talks to the port
directly with pyserial. Toggling DTR/RTS is what `pio run -t upload` does to
restart the board, so the capture starts at the boot banner instead of
mid-stream.

Writes two files:
  .doc/serial-stamped.txt   one "T+seconds\\tline" per line, for cadence math
  .doc/serial-raw.txt       byte-exact copy
"""
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BAUD = 115200


def parse_args(argv):
    """seconds [--port COM] [--baud N], all optional and order-independent."""
    seconds = 90.0
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
    import serial.tools.list_ports

    raw_path = ROOT / ".doc" / "serial-raw.txt"
    stamp_path = ROOT / ".doc" / "serial-stamped.txt"
    for p in (raw_path, stamp_path):
        p.parent.mkdir(parents=True, exist_ok=True)

    with serial.Serial(port, baud, timeout=0.2) as ser:
        # Classic ESP32-S3 auto-reset: DTR=RTS=1, then DTR=0 for ~100ms.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.dtr = True
        ser.rts = False
        time.sleep(0.1)
        ser.dtr = False
        ser.rts = False
        ser.reset_input_buffer()

        t0 = time.time()
        deadline = t0 + seconds
        buf = b""
        lines = []
        while time.time() < deadline:
            waiting = ser.in_waiting
            if waiting:
                buf += ser.read(waiting)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    stamp = time.time() - t0
                    lines.append("T+%.3f\t%s" % (stamp, line.decode("utf-8", "replace").rstrip("\r")))
        if buf:
            lines.append("T+%.3f\t%s" % (time.time() - t0, buf.decode("utf-8", "replace")))

    stamp_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    raw_path.write_bytes(b"\n".join(l.split("\t", 1)[1].encode("utf-8", "replace") for l in lines) + b"\n")
    print("read %d lines -> %s" % (len(lines), stamp_path))
    print("---- stamped ----")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
