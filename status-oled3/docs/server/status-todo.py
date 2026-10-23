#!/usr/bin/env python3
"""status-todo.py -- publish the todo board as one flat JSON document (rlcd-workbench, M3).

Reads a hand-edited source file, normalizes it into the 17-field flat contract
agreed in rlcd-workbench/docs/adr/0006, and atomically replaces
/var/www/status/todo.json. Run by crontab every 5 minutes as root; nothing stays
resident.

Source shape (hand-edited over ssh):

    {
      "items": [
        {"text": "交周报",   "due": "2026-10-23 18:00", "done": false},
        {"text": "买牛奶",   "due": "2026-10-24 09:00", "done": false},
        {"text": "整理照片", "due": "",                 "done": true}
      ],
      "trips": ["2026-10-22", "2026-10-22", "2026-10-24"]
    }

Six rules that are not obvious and must survive edits:

  1. EVERY run appends one line to the log, including runs that published
     nothing. "No anomaly" is information and has to be written down.

  2. The output is a complete valid JSON document or nothing at all. Write a
     temp file in the same directory, re-parse it, then os.replace() -- atomic
     within one directory, so the device never reads a partial file.

  3. Every field is a flat scalar: strings and bare integers only. The device
     reads this with a hand-written flat parser (src/jsonfield.cpp) that
     understands quoted strings and digits, and nothing else. No arrays, no
     nested objects, no booleans. `done` therefore travels as an integer count,
     not as per-item flags.

  4. All 17 fields are always present. A slot with nothing in it is an empty
     string, never an omitted key -- a missing key fails the device's
     "document looks complete" check for the WHOLE document, which would blank
     out the panel instead of one line.

  5. itemN_text is truncated to 12 characters HERE, not on the device. The
     budget is measured, not guessed: in the 192px right column the row is
     box(12) + gap(4) + text + gap(4) + due(23), leaving 149px for text, and one
     CJK glyph in wqy12 is 12px -> 12.4 glyphs (docs/adr/0006).

  6. itemN_due is passed through VERBATIM, garbage included. Deliberately the
     opposite of rule 5: a bad date is a device-side rendering problem and the
     device has to tolerate it anyway (assertion A3 requires exactly that --
     one malformed due must drop that one row, keep the other five, and raise a
     footer warning). Sanitizing it here would make that path untestable.

If the source file is missing or unparseable the script logs the reason, leaves
the previously published file untouched and exits non-zero. Publishing zeros
would render as "no todos" -- indistinguishable from a real empty board, and
wrong. Leaving the old file lets the device age it out and show "stale", which
is the truth.
"""

import argparse
import datetime
import json
import os
import sys
import tempfile

# Deployment paths. Overridable by environment so the same script can be
# self-tested off-Linux without touching /var.
OUT_DIR = os.environ.get("STATUS_OUT_DIR", "/var/www/status")
STATE_DIR = os.environ.get("STATUS_STATE_DIR", "/var/lib/status-todo")
LOG_FILE = os.environ.get("STATUS_LOG_FILE", "/var/log/status-todo.log")
OUT_FILE = os.path.join(OUT_DIR, "todo.json")
SOURCE_FILE = os.environ.get("STATUS_TODO_SOURCE", os.path.join(STATE_DIR, "source.json"))

# The contract, in emit order. Adding a field here without adding it to the
# device parser is a contract break, and this list is where that shows up.
FIELDS = [
    "generated_at", "total", "done",
    "item1_text", "item1_due",
    "item2_text", "item2_due",
    "item3_text", "item3_due",
    "item4_text", "item4_due",
    "item5_text", "item5_due",
    "item6_text", "item6_due",
    "days_month", "days",
]

SLOTS = 6
TEXT_LIMIT = 12


def log(msg):
    line = "%s %s\n" % (datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S"), msg)
    try:
        os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line)
    except OSError:
        pass
    sys.stderr.write(line)


def trim_text(text, limit=TEXT_LIMIT):
    text = " ".join(str(text).split())
    if len(text) <= limit:
        return text
    return text[:limit]


def read_source(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def normalize(src, now=None):
    """Turn the hand-edited source into the flat contract. Never raises on
    per-item garbage: bad entries are dropped and reported in `notes`."""
    now = now or datetime.datetime.now()
    notes = []
    items = src.get("items") or []
    if not isinstance(items, list):
        raise ValueError("items must be a list")

    good = []
    for i, it in enumerate(items):
        if not isinstance(it, dict):
            notes.append("item %d: not an object, dropped" % (i + 1))
            continue
        text = trim_text(it.get("text", ""))
        if not text:
            notes.append("item %d: empty text, dropped" % (i + 1))
            continue
        good.append({
            "text": text,
            "due": str(it.get("due", "")).strip(),
            "done": bool(it.get("done", False)),
        })

    # Pending first, then by due string. The due strings are "YYYY-MM-DD HH:MM",
    # which sorts correctly as text; the empty string sorts first, so items
    # without a due would jump ahead -- shoved to the back explicitly instead.
    good.sort(key=lambda x: (x["done"], x["due"] == "", x["due"]))

    doc = {"generated_at": now.astimezone(datetime.timezone.utc)
           .strftime("%Y-%m-%dT%H:%M:%SZ")}
    doc["total"] = len(good)
    doc["done"] = sum(1 for x in good if x["done"])

    # Only *pending* items go into the six slots.
    #
    # The contract carries just the counters (total / done), not a per-item done
    # flag, so anything parked in a slot gets drawn with an empty checkbox.  A
    # completed item in a slot would therefore make the screen assert "not done"
    # about something that is done -- the kind of lie that is invisible until
    # somebody reads the list.  Slots are the pending queue; total and done stay
    # true counters and may both exceed SLOTS.
    pending = [x for x in good if not x["done"]]
    for slot in range(SLOTS):
        if slot < len(pending):
            doc["item%d_text" % (slot + 1)] = pending[slot]["text"]
            doc["item%d_due" % (slot + 1)] = pending[slot]["due"]
        else:
            doc["item%d_text" % (slot + 1)] = ""
            doc["item%d_due" % (slot + 1)] = ""

    trips = src.get("trips") or []
    if not isinstance(trips, list):
        raise ValueError("trips must be a list")

    month = now.strftime("%Y-%m")
    counts = {}
    bad_trips = 0
    for raw in trips:
        try:
            d = datetime.datetime.strptime(str(raw).strip(), "%Y-%m-%d")
        except ValueError:
            bad_trips += 1
            continue
        if d.strftime("%Y-%m") != month:
            continue
        counts[d.day] = counts.get(d.day, 0) + 1
    if bad_trips:
        notes.append("%d trip date(s) unparseable, skipped" % bad_trips)

    doc["days_month"] = month
    doc["days"] = ",".join("%d:%d" % (k, counts[k]) for k in sorted(counts))
    return doc, notes


def check_contract(doc):
    """Cheap but complete: exact key set, every value a flat scalar."""
    problems = []
    if set(doc) != set(FIELDS):
        missing = sorted(set(FIELDS) - set(doc))
        extra = sorted(set(doc) - set(FIELDS))
        if missing:
            problems.append("missing: %s" % ",".join(missing))
        if extra:
            problems.append("unexpected: %s" % ",".join(extra))
    for k, v in doc.items():
        if isinstance(v, bool) or isinstance(v, (list, dict)) or v is None:
            problems.append("%s: not a flat scalar (%r)" % (k, type(v).__name__))
    return problems


def render(doc):
    return json.dumps(doc, ensure_ascii=False, separators=(",", ":")) + "\n"


def emit(doc):
    body = render(doc)
    os.makedirs(OUT_DIR, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=OUT_DIR, prefix=".todo.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        with open(tmp, encoding="utf-8") as f:
            json.load(f)
        os.replace(tmp, OUT_FILE)

        # mkstemp() creates 0600 while the other two documents in this
        # directory are 0644. nginx runs as www-data, so without widening the
        # mode the file exists but every request 404s -- verified on the live
        # server 2026-10-23. Widen AFTER the rename: doing it before would
        # publish a half-written file under a readable mode.
        os.chmod(OUT_FILE, 0o644)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return len(body.encode("utf-8"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true",
                    help="build a synthetic document, print it, write nothing")
    ap.add_argument("--dry-run", action="store_true",
                    help="normalize the real source, print it, write nothing")
    args = ap.parse_args()

    # Under cron stdout is a pipe and the locale may be POSIX; force UTF-8 so a
    # redirected run cannot mangle the JSON. The published file is written with
    # an explicit encoding, but --selftest / --dry-run only print.
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    if args.selftest:
        src = {
            "items": [
                {"text": "交周报", "due": "2026-10-23 18:00"},
                {"text": "买牛奶", "due": "2026-10-24 09:00"},
                {"text": "缴费", "due": "2026-10-21 09:00"},
                {"text": "换滤芯", "due": "2026-10-26 12:00"},
                {"text": "取快递", "due": "2026-10-22 20:00"},
                {"text": "整理照片", "due": "2026-10-28 00:00", "done": True},
                {"text": "第六条无期限", "due": ""},
                {"text": "   ", "due": ""},
            ],
            "trips": ["2026-10-22", "2026-10-22", "2026-10-24", "2026-10-30",
                      "2026-11-02", "not-a-date"],
        }
        now = datetime.datetime(2026, 10, 22, 15, 42, 0)
        doc, notes = normalize(src, now)
        problems = check_contract(doc)
        body = render(doc)
        print(body, end="")
        print("fields=%d bytes=%d slots=%d total=%d done=%d days=%r"
              % (len(doc), len(body.encode("utf-8")), SLOTS,
                 doc["total"], doc["done"], doc["days"]))
        if notes:
            print("notes: %s" % "; ".join(notes))
        if problems:
            print("CONTRACT FAIL: %s" % "; ".join(problems))
            return 1

        # Self-checking, not just self-printing: the numbers below are what the
        # device expects for the fixture above. If normalisation drifts, these
        # fail here instead of on the screen.
        expect = {
            "total": 7,             # 8 entries, one blank text dropped
            "done": 1,              # the done item sorts last -> falls out of the 6 slots
            "days_month": "2026-10",
            "days": "22:2,24:1,30:1",   # 11-02 is next month, the bad date is dropped
            "item1_text": "缴费",       # earliest due first
            # 5 条有期限的未完成 + 1 条无期限的未完成占满 6 个槽位；
            # 已完成的那条排在最后，落到第 7 位被挤掉。
            "item6_text": "第六条无期限",
        }
        bad = [(k, doc.get(k), v) for k, v in expect.items() if doc.get(k) != v]
        if bad:
            for k, got, want in bad:
                print("MISMATCH %s: got %r want %r" % (k, got, want))
            return 1
        if "item7_text" in doc:
            print("MISMATCH: slot 7 must not exist (contract is 6 slots)")
            return 1
        shown = [doc["item%d_text" % i] for i in range(1, 7)]
        if "整理照片" in shown:
            print("MISMATCH: a done item must not take a slot while pending ones remain")
            return 1
        print("selftest: OK (17 fields, %d bytes)" % len(body.encode("utf-8")))
        return 0

    try:
        src = read_source(SOURCE_FILE)
        doc, notes = normalize(src)
    except (OSError, ValueError) as exc:
        log("source unusable (%s): %s -- previous todo.json left in place"
            % (SOURCE_FILE, exc))
        return 1

    problems = check_contract(doc)
    if problems:
        log("contract broken, not publishing: %s" % "; ".join(problems))
        return 1

    if args.dry_run:
        print(render(doc), end="")
        for n in notes:
            print("note: %s" % n)
        return 0

    try:
        size = emit(doc)
    except OSError as exc:
        log("write failed: %s" % exc)
        return 1

    # One line per run, always. The device-visible numbers are in it so a
    # glance at the log answers "what did the screen just get?".
    extra = (" [%s]" % "; ".join(notes)) if notes else ""
    log("ok total=%d done=%d slots=%d days=%d bytes=%d%s"
        % (doc["total"], doc["done"], SLOTS,
           len([x for x in doc["days"].split(",") if x]), size, extra))
    return 0


if __name__ == "__main__":
    sys.exit(main())
