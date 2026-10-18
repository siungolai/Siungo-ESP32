#!/usr/bin/env python3
"""status-mailbox.py -- unread mail count every 5 minutes (status-oled3, M1b).

Writes /var/www/status/mailbox.json with the total unread count across the
configured accounts. Run by crontab as root; nothing stays resident.

Why this is a separate script from status-snapshot.py: different cadence
(5 min vs 1 min), different dependencies (IMAP vs /proc), different failure
mode. Merging them would make the 5-minute job wait on the 1-minute job
(docs/adr/0008 remark: "the two scripts stay separate").

Credential file: /etc/status-monitor-imap.json, root:root 0600.

    {
      "accounts": [
        {"host": "imap.qq.com", "port": 993, "user": "a@qq.com",
         "pass": "authorization-code", "tls": true}
      ]
    }

  NOT in git, and deliberately NOT in the .doc/deploy-secrets.txt style (that
  file has plaintext secrets committed to a private repo -- a pattern this
  project does not copy). When the file is missing the script still emits a
  valid document with "available": 0, so the mailbox screen can tell
  "not configured" apart from "0 unread" instead of freezing a stale number.

Same five rules as status-snapshot.py, and one addition:

  6. The unread count is a TOTAL across accounts, not per-account: one number
     fits the screen, per-account detail belongs in the log. When any account
     fails, the total is null and the failed count is reported -- a partial
     total would under-report and look like good news.

Field-shape note (docs/adr/0012): the device reads this document with a FLAT
parser (src/jsonfield.cpp, docs/adr/0010) that understands strings and numbers
only. Arrays and booleans are therefore emitted as numbers here:

  available      1 = credentials present and usable, 0 = not configured
  unread         integer, or null when unknown (not configured, or a failure)
  account_count  how many accounts are configured
  failed_count   how many of them failed this run

Changing a field name or type here without changing src/health.cpp is a
contract break.
"""

import argparse
import imaplib
import json
import os
import socket
import ssl
import sys
import time

CONF_FILE = os.environ.get("STATUS_IMAP_CONF", "/etc/status-monitor-imap.json")
OUT_DIR = os.environ.get("STATUS_OUT_DIR", "/var/www/status")
OUT_FILE = os.path.join(OUT_DIR, "mailbox.json")
LOG_FILE = os.environ.get("STATUS_LOG_FILE", "/var/log/status-mailbox.log")
IMAP_TIMEOUT = 15

FIELDS = ["generated_at", "available", "unread", "account_count", "failed_count"]


def log(msg):
    line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()), msg)
    try:
        with open(LOG_FILE, "a") as fh:
            fh.write(line)
    except OSError:
        pass


def load_accounts():
    """The configured accounts, or [] when the file is missing or unreadable."""
    try:
        with open(CONF_FILE) as fh:
            conf = json.load(fh)
    except (OSError, ValueError) as exc:
        log("CONF unreadable (%s): %s" % (CONF_FILE, exc))
        return []
    accounts = conf.get("accounts")
    if not isinstance(accounts, list):
        log("CONF malformed: 'accounts' is not a list")
        return []
    return [a for a in accounts
            if isinstance(a, dict) and a.get("host") and a.get("user")]


def count_unread(acct):
    """Unread count for one account.

    STATUS is issued against INBOX only: other folders (Sent, Drafts, custom
    labels) are not "unread mail" in the sense the screen means, and counting
    them would make the number move for reasons nobody cares about.
    """
    host = acct["host"]
    port = int(acct.get("port", 993))
    user = acct["user"]
    passwd = acct["pass"]
    use_tls = bool(acct.get("tls", True))

    ctx = ssl.create_default_context()
    try:
        if use_tls:
            conn = imaplib.IMAP4_SSL(host, port, ssl_context=ctx,
                                     timeout=IMAP_TIMEOUT)
        else:
            conn = imaplib.IMAP4(host, port, timeout=IMAP_TIMEOUT)
    except (OSError, imaplib.IMAP4.error, ssl.SSLError) as exc:
        raise RuntimeError("connect failed: %s" % exc)

    try:
        conn.login(user, passwd)
        typ, data = conn.select("INBOX", readonly=True)
        if typ != "OK":
            raise RuntimeError("select INBOX: %s" % (data,))
        # UNSEEN is the server's own notion of unread, so it matches what the
        # user's mail client shows. Counting messages locally would need a full
        # fetch and would drift from the client's view.
        typ, data = conn.search(None, "UNSEEN")
        if typ != "OK":
            raise RuntimeError("search UNSEEN: %s" % (data,))
        ids = data[0].split() if data and data[0] else []
        return len(ids)
    finally:
        try:
            conn.logout()
        except Exception:
            pass


def emit(doc):
    os.makedirs(OUT_DIR, exist_ok=True)
    tmp = os.path.join(OUT_DIR, ".mailbox.json.tmp")
    with open(tmp, "w") as fh:
        # Compact, for the same reason as status-snapshot.py: the device's
        # response buffer is 1 KiB and it rejects a body that fills it.
        json.dump(doc, fh, separators=(",", ":"))
        fh.write("\n")
    with open(tmp) as fh:  # validate before publishing
        json.load(fh)
    os.replace(tmp, OUT_FILE)  # atomic within one directory


def selftest():
    """Print both shapes (configured / not configured), writing nothing."""
    docs = [
        {"generated_at": "2026-10-17T12:00:00Z", "available": 1, "unread": 7,
         "account_count": 1, "failed_count": 0},
        {"generated_at": "2026-10-17T12:00:00Z", "available": 0, "unread": None,
         "account_count": 0, "failed_count": 0},
    ]
    for doc in docs:
        missing = [k for k in FIELDS if k not in doc]
        if missing:
            print("SELFTEST BROKEN: missing %s" % ", ".join(missing),
                  file=sys.stderr)
            return 1
        print(json.dumps(doc, indent=1))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="print both document shapes, write nothing")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    accounts = load_accounts()
    doc = {name: None for name in FIELDS}
    doc["generated_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    doc["account_count"] = len(accounts)
    doc["failed_count"] = 0

    if not accounts:
        doc["available"] = 0
        log("no accounts configured; emitting available=0")
    else:
        doc["available"] = 1
        counts = []
        for acct in accounts:
            try:
                counts.append(count_unread(acct))
                log("OK %s unread=%d" % (acct.get("user", "?"), counts[-1]))
            except Exception as exc:
                # A partial total would under-report and look like good news,
                # so one failure nulls the whole total (rule 6).
                doc["failed_count"] += 1
                log("FAIL %s: %s" % (acct.get("user", "?"), exc))
        doc["unread"] = sum(counts) if counts and doc["failed_count"] == 0 else None

    try:
        emit(doc)
    except OSError as exc:
        log("EMIT FAILED: %s, keeping previous document" % exc)
        return 1

    log("OK available=%s unread=%s accounts=%d failed=%d"
        % (doc["available"], doc["unread"], len(accounts), doc["failed_count"]))
    return 0


if __name__ == "__main__":
    # socket.timeout is what a dead IMAP host actually raises; let it through
    # as a per-account failure rather than killing the whole run.
    try:
        sys.exit(main())
    except socket.timeout:
        log("FATAL: socket timeout outside per-account handling")
        sys.exit(1)
