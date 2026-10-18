"""Compact a real status-snapshot.py selftest document and check its size.

    python tools/snapshot-size-probe.py

Why: the selftest output of status-snapshot.py is printed with indent=1 for
human reading, which is NOT what emit() writes. This script takes that real
document, compacts it the way emit() does, and reports the byte count against
config::HTTP_RESPONSE_MAX -- the number the device actually enforces.
"""
import json
import os
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = sys.executable
COLLECTOR = os.path.join(BASE, "docs", "server", "status-snapshot.py")
CHECKER = os.path.join(BASE, "docs", "server", "status-check.py")
SCRATCH = os.path.join(BASE, ".doc")


def main():
    if not os.path.isdir(SCRATCH):
        os.makedirs(SCRATCH)

    # The selftest prints to stdout; capture it without running anything.
    proc = subprocess.run([PYTHON, COLLECTOR, "--selftest"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        return 1
    doc = json.loads(proc.stdout.decode("utf-8"))

    # Written under the name emit() would use, so the checker recognises it.
    path = os.path.join(SCRATCH, "snapshot.json")
    with open(path, "w") as fh:
        json.dump(doc, fh, separators=(",", ":"))
        fh.write("\n")

    size = os.path.getsize(path)
    print("real document, compact form: %d bytes" % size)

    verdict = subprocess.run([PYTHON, CHECKER, path])
    os.remove(path)
    print("checker exit code: %d" % verdict.returncode)
    return verdict.returncode


if __name__ == "__main__":
    raise SystemExit(main())
