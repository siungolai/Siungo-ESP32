# status-oled3 nginx snippets (templates -- placeholders only, no real secrets)
#
# Both files are deployed by hand as root. The main site's
# /etc/nginx/sites-available/photography is NOT in any repository
# (W:\Github\Siungo-Website\README.md describes it only in prose), so these
# templates exist so that "copy it again" is possible instead of "remember it".
#
# Deployed layout:
#   /etc/nginx/conf.d/status-key.conf          <- this file's status-key part
#   /etc/nginx/sites-available/photography     <- the location block below,
#                                                 inserted before the SPA
#                                                 fallback location

# ---------------------------------------------------------------------------
# 1. /etc/nginx/conf.d/status-key.conf   (root:root 0600)
#
# The key lives in a root-only file, NOT in sites-available/photography.
# Two reasons:
#   * sites-available/photography is the file humans edit most often; a secret
#     in it spreads to the most-touched file on the box.
#   * nginx's master process reads configuration as root before dropping
#     privileges, so a 0600 include still loads fine.
#
# This file is loaded by nginx.conf's `include /etc/nginx/conf.d/*.conf;`
# inside http{}. Do NOT `include` it again from a server block: `map` is only
# legal in http{} and the second, server-level include is a hard error
# ("map directive is not allowed here", hit on 2026-10-17).
#
# The value is the main site's existing MONITOR_KEY (reused, not new -- see
# docs/adr/0008). It is the same secret the device sends in X-Monitor-Key.
#
# Documented limit, not an oversight: this key travels in the firmware, and
# firmware can be dumped off an ESP32. It stops casual scanning of a public
# machine's metrics; it does not stop someone holding the device. Anything
# that must resist that belongs behind the Go backend's auth, not here.
map $http_x_monitor_key $status_ok {
    default 0;
    "<MONITOR_KEY>" 1;
}

# ---------------------------------------------------------------------------
# 2. location block for /etc/nginx/sites-available/photography
#
# Insert BEFORE the SPA fallback (`location / { try_files ... }`), otherwise
# /status/... is served index.html instead of JSON and the device would parse
# HTML as data forever.
#
# THE AUTH CHECK MUST BE INSIDE THE INNER REGEX LOCATION. Verified the hard way
# on 2026-10-17: with `if ($status_ok = 0)` only in the outer `location
# /status/`, `/status/anything` returned 403 but `/status/snapshot.json`
# returned 200 *without* a key. Once a request matches a nested location, the
# outer location's `if` is not evaluated for it. The outer block therefore only
# does `return 403` as a catch-all for everything that is not one of the two
# whitelisted filenames.
#
# add_header is OVERRIDE, not additive (W:\Github\Siungo-Website\README.md:465-499:
# 6 headers x 4 levels). This location therefore repeats all six, or the
# responses under /status/ carry none of them.

location /status/ {
    root /var/www;
    autoindex off;

    # Whitelist by name, not by extension: only the two files the device
    # actually reads. `^/status/.+\.json$` would serve any future .json
    # dropped into the directory.
    location ~ ^/status/(snapshot|mailbox)\.json$ {
        if ($status_ok = 0) { return 403; }

        try_files $uri =404;

        default_type application/json;

        # The six security headers, repeated because add_header overrides.
        # Values copied from the main vhost; do not invent new ones here.
        add_header X-Frame-Options "SAMEORIGIN" always;
        add_header X-Content-Type-Options "nosniff" always;
        add_header Referrer-Policy "strict-origin-when-cross-origin" always;
        add_header Permissions-Policy "camera=(), microphone=(), geolocation=()" always;
        add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
        add_header Content-Security-Policy "default-src 'none'; frame-ancestors 'none'" always;

        # The device polls every 15 s; a minute of caching would make it read the
        # same file for four polls and then jump. No cache means the file's own
        # mtime is the freshness.
        add_header Cache-Control "no-store" always;

        access_log /var/log/nginx/status-access.log;
    }

    # Everything else under /status/ is refused without touching the filesystem.
    return 403;
}

# ---------------------------------------------------------------------------
# 3. crontab lines (root; `crontab -e`)
#
# ROOT'S CRONTAB HAS NO USER FIELD. The `root` column is only for
# /etc/crontab and /etc/cron.d/*. Writing `* * * * * root /usr/bin/python3 ...`
# into `crontab -e` makes cron run a command named `root`. (Hit on 2026-10-17.)
#
# Minute granularity for the snapshot, five minutes for the mailbox: the two
# jobs have different dependencies and cadences, so they stay separate
# (docs/adr/0008 remark).
#
# MAILTO="" suppresses per-job mail -- the scripts log to their own files, and
# cron mail to a mailbox that is being monitored for unread counts would make
# the unread number move for reasons that have nothing to do with mail.

MAILTO=""
* * * * * PROBE_TCP_TARGET=host:port /usr/bin/python3 /usr/local/bin/status-snapshot.py
#            ^ 出站 TCP 探针（svc_ma12）的目标。**必须在这里给**：脚本入库，
#              不能把第三方主机名＋非标端口写死在文件里。不设则该字段为 null，
#              设备端画「?」（没配）而不是红叉（挂了）。
*/5 * * * * /usr/bin/python3 /usr/local/bin/status-mailbox.py

# ---------------------------------------------------------------------------
# 4. post-deploy gate: status-check.py
#
# The field contract is frozen in docs/adr/0012. The device's parser
# (src/jsonfield.cpp, docs/adr/0010) has no error reporting: a renamed field
# shows up on the OLED as a permanent "-" with nothing anywhere else. This
# gate turns that into a non-zero exit at deploy time.
#
#   python3 /usr/local/bin/status-check.py /var/www/status/snapshot.json \
#                                        /var/www/status/mailbox.json
#
# Run it after every deploy AND after every edit to either collector. Field
# changes must be made in three places at once (docs/adr/0012 section 5):
# the ADR table, status-snapshot.py's FIELDS, and status-check.py's REQUIRED.
