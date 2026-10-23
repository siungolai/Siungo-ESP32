#!/usr/bin/env python3
"""status-snapshot.py -- one machine snapshot per minute (status-oled3, M1b).

Writes /var/www/status/snapshot.json from /proc, shutil.disk_usage and HTTPS
probes against the site itself. Run by crontab as root; nothing stays resident.

Why Python and not bash (this replaced a bash version):
  * df output, /proc/net/route and openssl's date format all need parsing that
    is locale- and version-sensitive. Python reads /proc directly.
  * The default route comes from /proc/net/route, so the `ip` command is not
    needed at all.
  * It can be self-tested off-Linux with --selftest, which is how the JSON
    contract got verified before anything touched the server.

Five rules that are not obvious and must survive edits:

  1. EVERY run appends one line to the log, including runs that collected
     nothing. photo-cleanup.sh failed silently for 34 days because its log
     looked normal while it collected nothing
     (W:\\Github\\Siungo-Website\\README.md:563-579). "No anomaly" is
     information and has to be written down.

  2. The output is a complete valid JSON document or nothing. The device
     defines valid data as "HTTP 200 + JSON parses + expected fields present"
     (docs/adr/0004), so a truncated write would read as a server failure.
     Hence: write a temp file, then os.replace() -- atomic within one
     directory, so a reader never sees a partial file.

  3. Numbers carry no units and no thousands separators. The device's parser
     (src/jsonfield.cpp) reads digits only and has no idea what "1.9G" means.
     Floats are scaled to integers (load1_x100) for the same reason.

  4. A field that could not be collected is emitted as null -- never omitted,
     never zero. "0" would be read as a real measurement; omitting it would
     fail the device's required-field check for the whole document. null lets
     the screen show "-" for one field while the rest still renders.

  5. Staleness is judged by the DEVICE on its own monotonic clock
     (docs/adr/0004). generated_at is informational only: the cron has minute
     granularity, so trusting it would make "the file is 40 s old" look normal.

Field-shape note (docs/adr/0012): the device reads this document with a FLAT
parser (src/jsonfield.cpp, docs/adr/0010) that understands strings and numbers
only. No arrays and no booleans are emitted anywhere below; service liveness
is 1/0 for that reason.
"""

import argparse
import calendar
import json
import os
import shutil
import socket
import ssl
import sys
import tempfile
import time
import urllib.request
import urllib.error

# Deployment paths. Overridable by environment so the same script can be
# self-tested off-Linux (see --selftest) without touching /var.
OUT_DIR = os.environ.get("STATUS_OUT_DIR", "/var/www/status")
LOG_FILE = os.environ.get("STATUS_LOG_FILE", "/var/log/status-snapshot.log")
STATE_DIR = os.environ.get("STATUS_STATE_DIR", "/var/lib/status-snapshot")
OUT_FILE = os.path.join(OUT_DIR, "snapshot.json")
FAIL_FILE = os.path.join(STATE_DIR, "failcount")
CPU_STATE_FILE = os.path.join(STATE_DIR, "cpu.stat")
STALE_SECS = 300
ALERT_AFTER_FAILS = 3
ALERT_URL = "https://127.0.0.1/api/settings/email/alert"

# Every field the device reads. Used two ways: as the emit order, and as the
# required-field check in --selftest. Adding a field here without adding it to
# the device's parser is a contract break, and this list is where that shows up.
FIELDS = [
    "generated_at", "sys_uptime_s",
    "cpu_pct",
    "mem_total_kb", "mem_used_kb", "mem_free_kb",
    "swap_total_kb", "swap_used_kb",
    "load1_x100",
    "disk_total_kb", "disk_used_kb", "disk_avail_kb",
    "net_if", "net_rx_bytes", "net_tx_bytes",
    "svc_web", "svc_select", "svc_wb",
    "svc_api", "svc_upload", "svc_ma12",
    "cert_days",
]

# Host header for every 127.0.0.1 probe. Must be a name the main vhost answers
# to, or nginx routes the request to another server block (see probe()).
PROBE_HOST = "www.siungo.top"

# The probes, as (field, url, host, expected_codes). The host is per-row now,
# not one global constant: select.siungo.top is a different vhost and pinning
# `Host: www.siungo.top` on its URL measured the wrong site (2026-10-23).
#
# svc_wb asks for /v1/models and accepts 401, on purpose. The obvious choice was
# /health, and it is wrong here: nginx's wb.siungo.top vhost only proxies /v1/
# and /admin/, so /health returns nginx's own 404 HTML, not the gateway's answer
# (measured 2026-10-23: /health -> 404, /v1/models -> 401 with a JSON body).
# 401 is a *better* liveness signal than 200 would be: it proves the request
# reached the gateway and its auth middleware ran. The codes that mean "broken"
# stay distinct -- 404 = nginx has no route, 502 = gateway down.
HTTP_PROBES = (
    ("svc_web", "https://127.0.0.1/", "www.siungo.top", (200,)),
    ("svc_select", "https://127.0.0.1/", "select.siungo.top", (200,)),
    ("svc_wb", "https://127.0.0.1/v1/models", "wb.siungo.top", (200, 401)),
    ("svc_api", "https://127.0.0.1/api/photos", "www.siungo.top", (200,)),
    ("svc_upload", "https://127.0.0.1/api/health", "www.siungo.top", (200,)),
)

# Outbound TCP probe. This one leaves the box, so its failure means something
# different from the HTTP probes': those fail when OUR stack stopped serving,
# this one fails when the far end is down OR we cannot get out at all. It also
# does not speak HTTP -- 11948 is a raw port, so "connected" is the whole
# signal and there is no status code to read.
def _tcp_probes():
    """出站 TCP 探针的目标从环境变量读，不写死在这个文件里。

    这是一个**第三方主机 + 非标端口**。把它写进公开仓库等于替别人广播
    「这个地址开放 11948」——本文件是入库的，所以只能留占位。

    格式 `host:port`，在 crontab 里给：`PROBE_TCP_TARGET=host:port`。
    没设就返回空元组，采集时会把这个字段写成 null（设备端画「?」而不是红叉：
    「没配」和「挂了」必须能分开）。
    """
    raw = os.environ.get("PROBE_TCP_TARGET", "").strip()
    if not raw or ":" not in raw:
        return ()
    host, _, port = raw.rpartition(":")
    try:
        return (("svc_ma12", host.strip(), int(port)),)
    except ValueError:
        return ()


TCP_PROBES = _tcp_probes()


def log(msg):
    """Append one line to the log.

    A logging failure must never take the snapshot down: if the log is
    unwritable the data is still worth publishing. This is the mirror image of
    the photo-cleanup.sh lesson (rule 1) -- failing to record success is bad,
    but refusing to produce data because the log is broken is worse.
    """
    line = "%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()), msg)
    try:
        with open(LOG_FILE, "a") as fh:
            fh.write(line)
    except OSError:
        pass


def num(value):
    """int or None. Anything non-numeric becomes None (rule 4)."""
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


# --------------------------------------------------------------------- probes
def read_first_line(path):
    try:
        with open(path) as fh:
            return fh.readline().strip()
    except OSError:
        return None


def meminfo():
    """kB values straight out of /proc/meminfo -- no parsing of df-style
    columns, no unit guessing."""
    out = {}
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                key, _, rest = line.partition(":")
                parts = rest.split()
                if parts:
                    out[key] = int(parts[0])
    except OSError:
        pass
    return out


def cpu_counters():
    """(total, idle) cumulative jiffies since boot from /proc/stat."""
    line = read_first_line("/proc/stat")
    if not line or not line.startswith("cpu "):
        return None, None
    parts = [int(x) for x in line.split()[1:]]
    while len(parts) < 8:
        parts.append(0)
    return sum(parts[:8]), parts[3]


def cpu_percent():
    """CPU busy percentage from the delta since the last run.

    Needs two samples, so the first run after boot emits None. The previous
    counters live in STATE_DIR; if that file is missing or unreadable the
    result is None rather than a bogus 0.
    """
    total, idle = cpu_counters()
    if total is None:
        return None
    prev = None
    try:
        with open(CPU_STATE_FILE) as fh:
            prev = tuple(int(x) for x in fh.read().split())
    except (OSError, ValueError):
        prev = None
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(CPU_STATE_FILE, "w") as fh:
            fh.write("%d %d\n" % (total, idle))
    except OSError:
        pass  # read-only state dir: still report this run's percentage
    if not prev or len(prev) != 2 or total <= prev[0]:
        return None
    d_total = total - prev[0]
    d_idle = idle - prev[1]
    if d_total <= 0:
        return None
    return int(round(100.0 * (d_total - d_idle) / d_total))


def default_iface():
    """Default-route interface from /proc/net/route, so `ip` is not needed.

    /proc/net/route is fixed-width hex; the destination 00000000 is the default
    route and field 1 is the interface name.
    """
    try:
        with open("/proc/net/route") as fh:
            fh.readline()  # header
            for line in fh:
                parts = line.split()
                if len(parts) > 2 and parts[1] == "00000000":
                    return parts[0]
    except OSError:
        pass
    return None


def net_bytes(iface):
    """(rx, tx) cumulative bytes for one interface, or (None, None)."""
    if not iface:
        return None, None
    base = "/sys/class/net/%s/statistics" % iface
    rx = read_first_line(os.path.join(base, "rx_bytes"))
    tx = read_first_line(os.path.join(base, "tx_bytes"))
    return num(rx), num(tx)


def probe(url, host=PROBE_HOST, timeout=5):
    """HTTP status of one probe, or None when the request itself failed.

    Goes to 127.0.0.1 where nginx listens, with certificate verification off:
    this asks "is the stack serving", and a self-signed-looking local cert is
    not what is being measured (docs/adr/0009 consequence 3).

    The Host header MUST be the real vhost name. Requesting
    https://127.0.0.1/api/photos sends `Host: 127.0.0.1`, which nginx routes to
    a different server block than the main site -- the request reaches the Go
    backend, the backend answers 200, but the response comes back as
    `404 page not found` because it was resolved against the wrong vhost
    (measured 2026-10-17: same URL with `Host: www.siungo.top` returns 200).
    Pinning it here keeps the probe measuring what it claims to measure.

    host is a parameter rather than a constant as of 2026-10-23: with more than
    one vhost on this box, one global PROBE_HOST meant every probe silently
    measured www.siungo.top no matter which name was in the URL.
    """
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(url, headers={"Host": host})
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status
    except urllib.error.HTTPError as exc:
        return exc.code
    except Exception:
        return None


def probe_tcp(host, port, timeout=3):
    """1 when a TCP connection to host:port succeeds, else 0.

    No bytes are sent and nothing is read: for a non-HTTP port, "the socket
    opened" is the only signal available, and it is enough to answer "is the
    far end reachable from here".

    Deliberately returns 0 rather than None on failure, because the device's
    field is a 1/0 flag. The cost is that "the far end is down" and "this box
    cannot get out" become the same value -- acceptable for a status light,
    and both deserve the same red mark (docs/adr/0014).
    """
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.close()
        return 1
    except Exception:
        return 0


def cert_days_left():
    """Days until the wildcard certificate expires, or None.

    The certificate is renewed BY HAND -- certbot has never managed it
    (W:\\Github\\Siungo-Website\\README.md:610-679) and the site once served an
    expired one for 17 days. Putting the number on the screen turns that risk
    into something visible instead of a surprise.

    Reads the certificate over a LOCAL TLS handshake, not
    ssl.get_server_certificate(): that helper hands the PEM to
    _test_decode_cert(), which only accepts a PATH and raises
    SSLError("Can't open file") on a string. Measured 2026-10-17 -- cert_days
    was null for exactly this reason.

    Parsing notAfter: the string ends in " GMT", so the epoch value is
    calendar.timegm(), NOT time.mktime(). mktime() reads the tuple as LOCAL
    time, which on this box (UTC+8) would report 8 hours less than the truth --
    harmless at day granularity, but wrong, and it would silently drift near
    the expiry boundary where the number actually matters.
    """
    try:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        with socket.create_connection(("127.0.0.1", 443), timeout=10) as raw:
            with ctx.wrap_socket(raw, server_hostname="www.siungo.top") as tls:
                der = tls.getpeercert(binary_form=True)
        if not der:
            return None
        # Decode through a temp file: _test_decode_cert takes a path only.
        with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as fh:
            fh.write(ssl.DER_cert_to_PEM_cert(der))
            pem_path = fh.name
        try:
            decoded = ssl._ssl._test_decode_cert(pem_path)  # noqa: SLF001
        finally:
            os.unlink(pem_path)
        not_after = decoded["notAfter"]  # e.g. 'Dec 22 20:00:54 2026 GMT'
        end = calendar.timegm(time.strptime(not_after, "%b %d %H:%M:%S %Y %Z"))
        return int((end - time.time()) // 86400)
    except Exception:
        return None


# ------------------------------------------------------------------ failures
def fail_count():
    try:
        with open(FAIL_FILE) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return 0


def bump_fail():
    n = fail_count() + 1
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(FAIL_FILE, "w") as fh:
            fh.write("%d\n" % n)
    except OSError:
        pass
    if n >= ALERT_AFTER_FAILS:
        # POST to the main site's machine endpoint -- the one path
        # middleware/csrf.go exempts from CSRF precisely because it is a
        # machine endpoint.
        log("ALERT: %d consecutive failures, mailing" % n)
        try:
            body = json.dumps({
                "subject": "status-monitor failing",
                "body": "status-snapshot.py has failed %d times in a row" % n,
            }).encode()
            req = urllib.request.Request(
                ALERT_URL, data=body,
                headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=10).read()
        except Exception as exc:
            log("ALERT FAILED: %s" % exc)
    return n


def clear_fail():
    n = fail_count()
    if n > 0:
        log("recovered after %d failures" % n)
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(FAIL_FILE, "w") as fh:
            fh.write("0\n")
    except OSError:
        pass


# --------------------------------------------------------------------- collect
def collect():
    """One snapshot dict. Never raises: a probe that fails becomes None."""
    snap = {name: None for name in FIELDS}
    failures = []

    mem = meminfo()
    if mem:
        total = mem.get("MemTotal")
        avail = mem.get("MemAvailable")
        snap["mem_total_kb"] = num(total)
        snap["mem_free_kb"] = num(avail)
        snap["mem_used_kb"] = (num(total) - num(avail)
                               if total is not None and avail is not None else None)
        snap["swap_total_kb"] = num(mem.get("SwapTotal"))
        snap["swap_used_kb"] = (num(mem.get("SwapTotal")) - num(mem.get("SwapFree"))
                                if mem.get("SwapTotal") is not None
                                and mem.get("SwapFree") is not None else None)
    else:
        failures.append("meminfo")

    snap["cpu_pct"] = cpu_percent()

    load = read_first_line("/proc/loadavg")
    if load:
        # Scaled to hundredths: the device's parser reads digits only.
        try:
            snap["load1_x100"] = int(round(float(load.split()[0]) * 100))
        except (ValueError, IndexError):
            failures.append("loadavg")

    up = read_first_line("/proc/uptime")
    if up:
        snap["sys_uptime_s"] = num(up.split()[0].split(".")[0])

    # Disk: shutil.disk_usage avoids parsing df's columns and locale entirely.
    try:
        usage = shutil.disk_usage("/")
        snap["disk_total_kb"] = usage.total // 1024
        snap["disk_used_kb"] = usage.used // 1024
        snap["disk_avail_kb"] = usage.free // 1024
    except OSError:
        failures.append("disk")

    iface = default_iface()
    snap["net_if"] = iface or "unknown"
    rx, tx = net_bytes(iface)
    snap["net_rx_bytes"] = rx
    snap["net_tx_bytes"] = tx
    if rx is None:
        failures.append("net:%s" % (iface or "no-route"))

    # Service liveness: each probe answers "is the whole stack serving", not
    # just "is the Go process alive".
    for key, url, host, expect in HTTP_PROBES:
        code = probe(url, host)
        snap[key] = 1 if code in expect else 0
        if code not in expect:
            failures.append("%s=%s" % (key, code))

    for key, host, port in TCP_PROBES:
        snap[key] = probe_tcp(host, port)
        if snap[key] != 1:
            failures.append("%s=unreachable(%s:%d)" % (key, host, port))
    if not TCP_PROBES:
        # 没配出站目标：字段仍然要有，值是 null。缺字段会让 status-check 报契约违规，
        # 而「本机没配这条探针」不是契约坏了。
        snap["svc_ma12"] = None

    snap["cert_days"] = cert_days_left()
    snap["generated_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    return snap, failures


def emit(snap):
    """Write the snapshot atomically, or leave the previous one in place."""
    os.makedirs(OUT_DIR, exist_ok=True)
    tmp = os.path.join(OUT_DIR, ".snapshot.json.tmp")
    with open(tmp, "w") as fh:
        # No indent: the device reads this document into a 1 KiB buffer
        # (config::HTTP_RESPONSE_MAX) and rejects anything that does not fit
        # (health.cpp fetchOnce() reports "body truncated at N bytes").
        # indent=1 roughly doubled the size and was one new field away from
        # permanently staling the screen; separators=(",", ":") is the compact
        # form and stays well inside the budget.
        json.dump(snap, fh, separators=(",", ":"))
        fh.write("\n")
    # Validate before publishing: a syntax error means the temp file is thrown
    # away and last minute's snapshot stays live. A stale but valid document
    # beats an invalid one.
    with open(tmp) as fh:
        json.load(fh)
    os.replace(tmp, OUT_FILE)  # atomic within one directory


def check_stale():
    try:
        age = time.time() - os.path.getmtime(OUT_FILE)
    except OSError:
        return
    if age > STALE_SECS:
        log("WARNING: snapshot is %ds old (threshold %ds)" % (int(age), STALE_SECS))


# --------------------------------------------------------------------- selftest
def selftest():
    """Print a snapshot built from synthetic data, touching nothing on disk.

    This is how the JSON contract was verified before the server was touched:
    the field names, the null behaviour and the required-field set are all
    observable here, on any machine.
    """
    snap = {name: None for name in FIELDS}
    snap.update({
        "generated_at": "2026-10-17T12:00:00Z",
        "sys_uptime_s": 237600,
        "cpu_pct": 7,
        "mem_total_kb": 1637856,
        "mem_used_kb": 421000,
        "mem_free_kb": 1216856,
        "swap_total_kb": 0,
        "swap_used_kb": 0,
        "load1_x100": 12,
        "disk_total_kb": 41251136,
        "disk_used_kb": 9873408,
        "disk_avail_kb": 29222400,
        "net_if": "eth0",
        "net_rx_bytes": 976056208,
        "net_tx_bytes": 779650684,
        "svc_web": 1,
        "svc_select": 1,
        "svc_wb": 1,
        "svc_api": 1,
        "svc_upload": 1,
        "svc_ma12": 1,
        "cert_days": 66,
    })
    print(json.dumps(snap, indent=1))
    missing = [k for k, v in snap.items() if v is None]
    if missing:
        print("note: null fields (screen shows '-'): %s" % ", ".join(missing),
              file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="print a synthetic snapshot, write nothing")
    ap.add_argument("--check", action="store_true",
                    help="report staleness of the live snapshot only")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.check:
        check_stale()
        return 0

    snap, failures = collect()
    try:
        emit(snap)
    except OSError as exc:
        log("EMIT FAILED: %s, keeping previous snapshot" % exc)
        bump_fail()
        return 1

    log("OK fails=%s bytes=%d generated_at=%s"
        % (len(failures), os.path.getsize(OUT_FILE), snap["generated_at"]))
    for item in failures:
        log("  probe failed: %s" % item)
    clear_fail()
    check_stale()
    return 0


if __name__ == "__main__":
    sys.exit(main())
