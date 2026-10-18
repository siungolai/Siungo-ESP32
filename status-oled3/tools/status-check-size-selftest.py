"""Exercise the size gate added to docs/server/status-check.py.

    python tools/status-check-size-selftest.py

The gate is the one that catches the failure the firmware cannot report: a
document that grows past config::HTTP_RESPONSE_MAX is truncated by the device
and then rejected as incomplete, which looks exactly like a dead server.
Three cases: comfortably inside, close enough to warn, and over the limit.
"""
import json
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(BASE, "docs", "server", "status-check.py")

import importlib.util

_spec = importlib.util.spec_from_file_location("status_check", CHECKER)
sc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sc)

FIELDS = [
    "generated_at", "sys_uptime_s", "cpu_pct",
    "mem_total_kb", "mem_used_kb", "mem_free_kb",
    "swap_total_kb", "swap_used_kb", "load1_x100",
    "disk_total_kb", "disk_used_kb", "disk_avail_kb",
    "net_if", "net_rx_bytes", "net_tx_bytes",
    "svc_web", "svc_api", "svc_upload", "cert_days",
]


def make_doc(net_if="eth0"):
    return {
        "generated_at": "2026-10-18T03:00:00Z", "sys_uptime_s": 12345,
        "cpu_pct": 3, "mem_total_kb": 1000, "mem_used_kb": 500,
        "mem_free_kb": 500, "swap_total_kb": 0, "swap_used_kb": 0,
        "load1_x100": 42, "disk_total_kb": 900, "disk_used_kb": 400,
        "disk_avail_kb": 500, "net_if": net_if, "net_rx_bytes": 1,
        "net_tx_bytes": 2, "svc_web": 1, "svc_api": 1, "svc_upload": 1,
        "cert_days": 30,
    }


def run(doc):
    """Write the document where emit() would and run the checker on it."""
    # .doc/ is gitignored and writable, which is what the other selftests use
    # for scratch output.
    tmp = os.path.join(BASE, ".doc", "tmp-size-gate")
    if not os.path.isdir(tmp):
        os.makedirs(tmp)
    path = os.path.join(tmp, "snapshot.json")
    with open(path, "w") as fh:
        json.dump(doc, fh, separators=(",", ":"))
        fh.write("\n")
    try:
        verdict = sc.check(path)
    finally:
        os.remove(path)
        os.rmdir(tmp)
    return verdict


def main():
    failures = []
    limit = sc.DEVICE_BUFFER_BYTES

    normal = make_doc()
    size = len(json.dumps(normal, separators=(",", ":")).encode()) + 1
    print("normal   %4d/%d bytes -> %s" % (size, limit, run(normal)))
    if run(normal) is not True:
        failures.append("a document well inside the buffer was rejected")

    # Pad until the serialised body is ~92% of the budget: inside, but close.
    pad = int(limit * 0.92) - size
    near = make_doc(net_if="eth0" + "x" * max(pad, 0))
    size_near = len(json.dumps(near, separators=(",", ":")).encode()) + 1
    got = run(near)
    print("near     %4d/%d bytes -> %s" % (size_near, limit, got))
    if got is not False:
        failures.append("a document at 92%% of the buffer was not flagged")

    # And one that is past it.
    pad = limit + 50 - size
    over = make_doc(net_if="eth0" + "x" * max(pad, 0))
    size_over = len(json.dumps(over, separators=(",", ":")).encode()) + 1
    got = run(over)
    print("over     %4d/%d bytes -> %s" % (size_over, limit, got))
    if got is not False:
        failures.append("a document past the buffer was not rejected")

    print()
    if failures:
        print("SELFTEST FAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("OK: the size gate accepts a fitting document and rejects the rest")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
