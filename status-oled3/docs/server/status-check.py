#!/usr/bin/env python3
"""status-check.py -- contract checker for the status-oled3 JSON documents.

The device decides "is this usable data" from three facts (docs/adr/0004):
HTTP 200, the body parses as JSON, and the expected fields are present. This
script checks the second and third facts for EVERY document without needing
the server or the device, so the field contract can be verified before either
side is touched. `snapshot.json` and `mailbox.json` come from
status-oled3; `todo.json` comes from rlcd-workbench (status-todo.py).

Usage:

    python3 status-check.py [snapshot.json ...] [mailbox.json ...]

With no arguments it prints the required field list for each document and
exits. With file arguments it validates each one and exits non-zero on the
first failure, which makes it usable as a pre-deploy gate:

    scp snapshot.json ...
    python3 status-check.py /var/www/status/snapshot.json \
                             /var/www/status/mailbox.json \
                             /var/www/status/todo.json

Why this exists: the device's parser is a hand-written flat reader
(src/jsonfield.cpp, docs/adr/0010). It has no error messages. If a field is
renamed on the server side, the device shows "-" forever and nobody knows why;
this script turns that silent failure into a one-line message at deploy time.

It also checks the serialised size against the firmware's 1 KiB response
buffer (src/config.h HTTP_RESPONSE_MAX): a document that grows past it is
truncated by the device and then rejected, which looks exactly like a dead
server. That is the failure mode this script is best at catching, because the
device cannot report it.
"""

import json
import sys

# Required fields per document. A field may be null (a probe that could not
# collect it -- status-snapshot.py rule 4), but it must EXIST.
REQUIRED = {
    "snapshot.json": [
        "generated_at", "sys_uptime_s", "cpu_pct",
        "mem_total_kb", "mem_used_kb", "mem_free_kb",
        "swap_total_kb", "swap_used_kb", "load1_x100",
        "disk_total_kb", "disk_used_kb", "disk_avail_kb",
        "net_if", "net_rx_bytes", "net_tx_bytes",
        "svc_web", "svc_select", "svc_wb",
        "svc_api", "svc_upload", "svc_ma12",
        "cert_days",
    ],
    "mailbox.json": [
        "generated_at", "available", "unread",
        "account_count", "failed_count",
    ],
    # rlcd-workbench (docs/adr/0006). Six fixed slots plus five scalars. An
    # empty slot is an empty string, never an omitted key -- the device judges
    # the whole document by "are all fields here", so one missing key blanks
    # the panel instead of one row.
    "todo.json": [
        "generated_at", "total", "done",
        "item1_text", "item1_due",
        "item2_text", "item2_due",
        "item3_text", "item3_due",
        "item4_text", "item4_due",
        "item5_text", "item5_due",
        "item6_text", "item6_due",
        "days_month", "days",
    ],
}

# Types the device's flat parser can actually read (docs/adr/0010): a quoted
# string, a JSON number, or null. Anything else (array, object, boolean)
# silently becomes "" / false on the device.
ALLOWED_TYPES = (str, int, float, type(None))

# The device's response buffer (src/config.h HTTP_RESPONSE_MAX). A body at or
# past this size is truncated by the firmware and then rejected as incomplete
# (health.cpp fetchOnce() now says so explicitly), which presents as a
# permanently stale screen with no cause in the log. The server side can see
# it coming: this is the number that must not be reached.
#
# Keep in sync with src/config.h. The check uses the serialised byte length,
# including the trailing newline emit() writes, because that is what the
# firmware reads.
DEVICE_BUFFER_BYTES = 1024


def usage():
    print(__doc__.strip())
    return 0


def check(path):
    import os
    kind = os.path.basename(path)
    if kind not in REQUIRED:
        print("SKIP %s: unknown document name (expected %s)"
              % (path, " or ".join(sorted(REQUIRED))))
        return None

    try:
        # encoding= must be explicit. The documents carry Chinese (todo.json
        # text fields), and on a machine whose default text encoding is not
        # UTF-8 the plain open() fails with a confusing codec error instead of
        # reading the file. The server happens to default to UTF-8; the
        # deploy-time self-test may not.
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        print("FAIL %s: %s" % (path, exc))
        return False

    if not isinstance(doc, dict):
        print("FAIL %s: top level is %s, not an object"
              % (path, type(doc).__name__))
        return False

    problems = []
    for key in REQUIRED[kind]:
        if key not in doc:
            problems.append("missing field: %s" % key)
            continue
        value = doc[key]
        if isinstance(value, bool):
            # bool is a subclass of int in Python; the device has no boolean.
            problems.append("boolean field (device reads 0/1): %s" % key)
        elif not isinstance(value, ALLOWED_TYPES):
            problems.append("unreadable type %s: %s"
                            % (type(value).__name__, key))

    # Size, measured the way the firmware measures it: the compact bytes that
    # emit() writes, newline included. Warn at 90% of the budget so there is
    # room to notice before the document actually stops fitting.
    # ensure_ascii=False matters now that a document carries Chinese. Without
    # it json.dumps escapes every CJK glyph to \uXXXX, and the measured size
    # overstates what the device receives by ~20% (measured 2026-10-23:
    # todo.json 472 real bytes measured as 556). Conservative in the safe
    # direction, but it is not the number the firmware sees, and a false
    # "close to the limit" warning is worse than no warning.
    serialised = json.dumps(doc, ensure_ascii=False,
                            separators=(",", ":")).encode("utf-8") + b"\n"
    size_note = "%d/%d bytes" % (len(serialised), DEVICE_BUFFER_BYTES)
    if len(serialised) >= DEVICE_BUFFER_BYTES:
        problems.append("body does not fit the device buffer: %s" % size_note)
    elif len(serialised) * 10 >= DEVICE_BUFFER_BYTES * 9:
        problems.append("body close to the device buffer limit: %s" % size_note)

    if problems:
        print("FAIL %s:" % path)
        for item in problems:
            print("  - %s" % item)
        return False

    nulls = [k for k in REQUIRED[kind] if doc[k] is None]
    note = (" (null: %s)" % ", ".join(nulls)) if nulls else ""
    print("OK   %s: %d fields, %s%s" % (path, len(REQUIRED[kind]),
                                        size_note, note))
    return True


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        if not argv:
            for kind in sorted(REQUIRED):
                print("%s requires %d fields:" % (kind, len(REQUIRED[kind])))
                for key in REQUIRED[kind]:
                    print("  %s" % key)
                print()
            return 0
        return usage()

    results = [check(path) for path in argv]
    # None means "not one of our documents", which is not a failure.
    verdicts = [r for r in results if r is not None]
    if not verdicts:
        return 0
    return 0 if all(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
