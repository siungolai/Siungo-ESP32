"""Button press/long-press check over serial: one event per physical press.

    python tools/serial-button-test.py [seconds] [--port COM4] [--baud 115200]

Dumps the button state machine's own log lines. The bug this verifies was:
holding a button re-fired EVENT_LONG_PRESS every ~20 ms (BUTTON_DEBOUNCE_MS),
visible as a flood of "btn A long: no action" lines. A human must press and
hold the button; this script only captures what the firmware prints.

Run it while: press A briefly, then hold A for ~3 s, then release.
"""
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BAUD = 115200


def parse_args(argv):
    """seconds [--port COM] [--baud N], all optional and order-independent."""
    seconds = 20.0
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

    stamp = ROOT / ".doc" / "serial-button.txt"
    stamp.parent.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    lines = []
    with serial.Serial(port, baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        deadline = t0 + seconds
        buf = b""
        while time.time() < deadline:
            waiting = ser.in_waiting
            if waiting:
                buf += ser.read(waiting)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    lines.append("T+%7.3f  %s" % (
                        time.time() - t0,
                        line.decode("utf-8", "replace").rstrip("\r")))
    stamp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))

    # Long-press lines: a correct build prints at most one per hold.
    longs = [l for l in lines if "long: no action" in l]
    print("----")
    print("long-press lines: %d" % len(longs))
    if len(longs) > 1:
        # Are they clustered (one hold, re-fired) or spread (two holds)?
        times = [float(re.match(r"T\+([\d.]+)", l).group(1)) for l in longs]
        spread = max(times) - min(times)
        print("FAIL: %d long-press events in %.3f s" % (len(longs), spread))
        print("      one hold must produce exactly one; the rest are re-fires")
        return 1
    print("OK: at most one long-press event per capture window")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
