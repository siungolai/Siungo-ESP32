"""Write two demo documents and run docs/server/status-check.py on them.

    python tools/status-check-demo.py

The point of running the real checker rather than trusting it by inspection:
the size gate measures the body the way the firmware measures it (compact
separators, newline included), so this is the one place where the server side
and the device side of the 1 KiB budget are checked against each other.
"""
import json
import os
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = sys.executable
CHECKER = os.path.join(BASE, "docs", "server", "status-check.py")

SNAPSHOT = {
    "generated_at": "2026-10-18T04:00:00Z", "sys_uptime_s": 999999,
    "cpu_pct": 7, "mem_total_kb": 1000, "mem_used_kb": 512, "mem_free_kb": 488,
    "swap_total_kb": 0, "swap_used_kb": 0, "load1_x100": 53,
    "disk_total_kb": 900, "disk_used_kb": 431, "disk_avail_kb": 469,
    "net_if": "eth0", "net_rx_bytes": 123456, "net_tx_bytes": 654321,
    "svc_web": 1, "svc_api": 1, "svc_upload": 1, "cert_days": 87,
}
MAILBOX = {
    "generated_at": "2026-10-18T04:00:00Z", "available": 1, "unread": 3,
    "account_count": 2, "failed_count": 0,
}


def main():
    # .doc/ is gitignored, which is what every other scratch output uses.
    out = os.path.join(BASE, ".doc", "contract-demo")
    if not os.path.isdir(out):
        os.makedirs(out)
    written = []
    for name, doc in (("snapshot.json", SNAPSHOT), ("mailbox.json", MAILBOX)):
        path = os.path.join(out, name)
        with open(path, "w") as fh:
            json.dump(doc, fh, separators=(",", ":"))
            fh.write("\n")
        written.append(path)

    proc = subprocess.run([PYTHON, CHECKER] + written)
    print("checker exit code: %d" % proc.returncode)
    for path in written:
        os.remove(path)
    os.rmdir(out)
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
