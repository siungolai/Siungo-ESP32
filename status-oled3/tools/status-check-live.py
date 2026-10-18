"""Contract check on a freshly collected real document pair.

    python tools/status-check-live.py

Runs the collector and the mailbox reader in --selftest mode, writes both
documents to .doc/ under the exact names status-check.py expects, and reports
the contract verdict plus the byte counts the device will actually receive.
"""
import json
import os
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHON = sys.executable
SERVER = os.path.join(BASE, "docs", "server", "status-snapshot.py")
MAILBOX = os.path.join(BASE, "docs", "server", "status-mailbox.py")
CHECKER = os.path.join(BASE, "docs", "server", "status-check.py")
SCRATCH = os.path.join(BASE, ".doc")

CASES = [
    ("snapshot", SERVER, "snapshot.json"),
    ("mailbox", MAILBOX, "mailbox.json"),
]


def load_docs(producer):
    """Return every JSON document the producer prints in --selftest mode.

    status-mailbox.py deliberately prints two shapes (configured and not
    configured), so the output is a stream of documents, not one.
    """
    proc = subprocess.run([PYTHON, producer, "--selftest"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        return None
    return parse_stream(proc.stdout.decode("utf-8"))


def parse_stream(text):
    docs = []
    pos = 0
    while pos < len(text):
        while pos < len(text) and text[pos] in " \t\r\n":
            pos += 1
        if pos >= len(text):
            break
        doc, end = json.JSONDecoder().raw_decode(text, pos)
        docs.append(doc)
        pos = end
    return docs


def main():
    if not os.path.isdir(SCRATCH):
        os.makedirs(SCRATCH)
    worst = 0
    for name, producer, filename in CASES:
        docs = load_docs(producer)
        if docs is None:
            return 1
        for i, doc in enumerate(docs):
            # Every document of one kind goes under the same name; the checker
            # reads one path at a time, so run it once per shape.
            path = os.path.join(SCRATCH, filename)
            with open(path, "w") as fh:
                json.dump(doc, fh, separators=(",", ":"))
                fh.write("\n")
            size = os.path.getsize(path)
            verdict = subprocess.run([PYTHON, CHECKER, path])
            os.remove(path)
            worst = max(worst, verdict.returncode)
            print("  -> %s shape %d: %d fields, %d bytes on the wire" % (
                name, i + 1, len(doc), size))
    return worst


if __name__ == "__main__":
    raise SystemExit(main())
