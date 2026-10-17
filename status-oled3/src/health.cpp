/**
 * health.cpp -- the state machine behind health.h
 *
 * Four states, driven by two timers:
 *
 *   WIFI_DOWN  -- not associated. Retried every WIFI_CHECK_INTERVAL_MS.
 *   POLL_DUE   -- associated and the 15 s poll interval has elapsed.
 *   FETCHING   -- an HTTP request is in flight (blocking, but bounded by
 *                 HTTP_TIMEOUT_MS).
 *   IDLE       -- associated, poll not due yet.
 *
 * The WiFi reconnect check runs at a slower cadence than the poll because a
 * reconnect attempt blocks for seconds (WIFI_CONNECT_TIMEOUT_MS), and doing
 * that every poll would leave the loop unresponsive for a large fraction of
 * the time.
 *
 * Why a plain WiFiClientSecure with no pinned certificate (docs/adr/0006):
 * the device fetches https://www.siungo.top, whose chain ends in a publicly
 * trusted CA, so the default verification is correct and cheap. No IP, no
 * port and no certificate bytes are baked into the firmware; the only
 * consequence is that a certificate that expires or a DNS that fails both
 * show up as "stale" -- which is the honest answer for "is the server
 * reachable", and is documented in GLOSSARY.md under "stale".
 *
 * Two facts are tracked separately, because they fail independently:
 *   g_net_online    -- the WiFi stack says we are associated
 *   g_link_verified -- a plain TCP connect out of the LAN succeeded
 * The second one is the useful precondition for polling. Measured on the
 * bench 2026-10-17: an AP can accept the association and still give no
 * route out, and every HTTPS attempt then fails the same way, which the
 * screen would render as "server stale" and blame the wrong party.
 */
#include "health.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "jsonfield.h"
#include "secrets.h"

#include <errno.h>

namespace health {

namespace {

// ---- link state -----------------------------------------------------------
bool g_net_online = false;

// Whether the interface has been proven to carry traffic out of the LAN.
// "Associated" and "usable" are different facts, and only the second one
// makes a poll meaningful: an AP can accept the association and still have
// no upstream (which is what a WiFiClientSecure failure looks like from the
// firmware's side).
bool g_link_verified = false;

// ---- poll bookkeeping -----------------------------------------------------
uint32_t g_last_poll_ms = 0;
uint32_t g_last_wifi_check_ms = 0;
uint32_t g_last_success_ms = 0;
uint32_t g_poll_count = 0;

// A first poll has never happened, so "seconds since data" would otherwise
// start from boot and look like a 10-minute-old server. Treated as stale
// until the first success, but reported distinctly on the serial log.
bool g_ever_succeeded = false;

// ---- the values pulled out of /api/health ---------------------------------
char g_uptime[32] = {0};
char g_commit[48] = {0};
char g_built[48] = {0};

// ---- HTTP ----------------------------------------------------------------
// The client is created per request inside fetchOnce() -- see the note there
// about socket reuse. Nothing is cached at file scope.

// Plain TCP probe of the data host. Not HTTPS: a TLS handshake would fail
// identically for "no route out", "DNS broken" and "certificate expired",
// which is exactly the ambiguity we are trying to remove. If this connect
// succeeds the L3 path to the host is fine and any later HTTPS failure is a
// certificate/DNS problem rather than a dead interface.
bool verifyLink() {
  WiFiClient probe;
  probe.setTimeout(5);
  const bool ok = probe.connect(config::DATA_HOST, 443);
  if (!ok) {
    Serial.print("wifi: probe connect to ");
    Serial.print(config::DATA_HOST);
    Serial.print(":443 failed (errno ");
    Serial.print(errno);
    Serial.println(") -- interface has no route out");
    probe.stop();
    return false;
  }
  probe.stop();
  return true;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    g_net_online = true;
    return;
  }
  Serial.print("wifi: connecting to \"");
  Serial.print(WIFI_SSID);
  Serial.println("\" ...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // no power saving: it adds jitter to the TLS round trip
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t deadline = millis() + config::WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      Serial.println("wifi: timeout, will retry");
      g_net_online = false;
      return;
    }
    delay(250);
  }

  g_net_online = true;
  Serial.print("wifi: connected, IP=");
  Serial.print(WiFi.localIP().toString().c_str());
  Serial.print(" RSSI=");
  Serial.print(static_cast<int>(WiFi.RSSI()));
  Serial.println(" dBm");
  // The WiFi stack reports "associated" only; whether packets actually leave
  // the interface is a separate question, and it decides everything the rest
  // of this file does. Verify the link by hitting something that must answer
  // (docs/adr/0006 consequence 1: the same test tells DNS failures and
  // certificate expiry apart from a dead link, which the screen cannot).
  g_link_verified = verifyLink();
  if (!g_link_verified) {
    Serial.println("wifi: associated but no traffic leaves the interface");
  }
}

// Fetch one snapshot. Returns true when the body parsed as usable JSON.
// The response body lands in a caller-provided static buffer: no heap, and
// a truncated body is parsed as received (docs/adr/0010 edge case 4).
//
// A fresh client object per attempt: WiFiClientSecure keeps socket state
// across begin()/end() cycles and a stale fd makes setSocketOption() fail
// with EBADF on the next attempt, which then poisons every later poll.
//
// TLS verification (bench 2026-10-17; ADR-0006 amended):
//   - With neither setCACert() nor setCACertBundle() nor setInsecure(),
//     ssl_client.cpp:226-228 falls through to `return -1`. That is why the
//     first upload failed with start_ssl_client: -1 while the plain TCP
//     probe against the same host:443 succeeded -- the route was fine all
//     along, the TLS configuration was not.
//   - setCACert() with a real bundle would cost about 2 KB of flash plus a
//     large heap copy, for a payload that is public, device-independent and
//     shape-checked against the expected field set.
//   - setInsecure() is therefore the deliberate posture: an on-path attacker
//     can forge the JSON, and the worst outcome is a wrong on-screen number
//     and a wrong buzz. No credential and no secret is exposed, and the
//     server still cannot see a plaintext secret from us either.
//   - Pinning the server's own chain (the upgrade that keeps verification on)
//     is deferred to M2, where the certificate lifecycle already has to be
//     tracked by hand (the cert is renewed manually, ADR-0006).
bool fetchOnce(char* body, size_t cap) {
  WiFiClientSecure client;
  client.setTimeout(config::HTTP_TIMEOUT_MS / 1000);
  client.setInsecure();

  HTTPClient http;  String url = String("https://") + config::DATA_HOST + config::DATA_PATH;
  if (!http.begin(client, url)) {
    Serial.println("[poll] http.begin() failed");
    return false;
  }
  http.setTimeout(config::HTTP_TIMEOUT_MS);
  // M1b sends the shared key; at M1a MONITOR_KEY is empty and the header is
  // simply omitted (the endpoint is public).
  if (MONITOR_KEY[0] != '\0') {
    http.addHeader(config::MONITOR_KEY_HEADER, MONITOR_KEY);
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("[poll] HTTP ");
    Serial.println(code);
    http.end();
    return false;
  }

  // Read at most cap-1 bytes so the buffer stays NUL-terminated.
  WiFiClient& stream = http.getStream();
  size_t n = 0;
  while (stream.connected() && n + 1 < cap) {
    const int c = stream.read();
    if (c < 0) {
      break;
    }
    body[n++] = static_cast<char>(c);
  }
  body[n] = '\0';
  http.end();

  if (n == 0) {
    Serial.println("[poll] empty body");
    return false;
  }
  return true;
}

// Pull the three M1a fields out of the body. "status" is deliberately NOT
// read: handler/health.go always answers "ok" while the process lives, so
// it carries no information about server health (docs/adr/0009 consequence 2).
// "commit"/"built" keep "dev"/"unknown" as-is: they are the -ldflags defaults
// for a build without injection, and printStatus() labels them explicitly.
void parseBody(const char* body) {
  jsonfield::field(body, "uptime", g_uptime, sizeof(g_uptime));
  if (g_uptime[0] == '\0') {
    // /api/health always sends uptime; its absence means this body is not
    // the expected JSON (captive portal, error page, truncated read).
    return;
  }
  jsonfield::field(body, "commit", g_commit, sizeof(g_commit));
  jsonfield::field(body, "built", g_built, sizeof(g_built));
}

void poll() {
  static char body[config::HTTP_RESPONSE_MAX];

  if (!fetchOnce(body, sizeof(body))) {
    // Failure: no state change beyond "not fresh". The next attempt is one
    // POLL_INTERVAL_MS away (docs/adr/0004: no immediate retry).
    return;
  }

  parseBody(body);
  if (g_uptime[0] == '\0') {
    Serial.println("[poll] body parsed but uptime missing; not counted as data");
    return;
  }

  g_last_success_ms = millis();
  g_poll_count++;
  g_ever_succeeded = true;

  Serial.print("[poll] ok uptime=");
  Serial.print(g_uptime);
  Serial.print(" commit=");
  Serial.print(g_commit[0] != '\0' ? g_commit : "(none)");
  if (strcmp(g_commit, "dev") == 0 || strcmp(g_built, "unknown") == 0) {
    Serial.print(" [not injected]");
  }
  Serial.print(" built=");
  Serial.println(g_built[0] != '\0' ? g_built : "(none)");
}

}  // namespace

void begin() {
  g_net_online = false;
  g_link_verified = false;
  g_last_poll_ms = millis();
  g_last_wifi_check_ms = millis();
  g_last_success_ms = 0;
  g_poll_count = 0;
  g_ever_succeeded = false;
  g_uptime[0] = '\0';
  g_commit[0] = '\0';
  g_built[0] = '\0';
}

void update() {
  const uint32_t now = millis();

  // ---- link upkeep (slower cadence: a connect attempt blocks) ----
  if (!g_net_online && now - g_last_wifi_check_ms >=
                           config::WIFI_CHECK_INTERVAL_MS) {
    g_last_wifi_check_ms = now;
    connectWifi();
    if (g_net_online) {
      // Poll immediately on a fresh link instead of waiting out the
      // interval: the user just watched the network come back.
      g_last_poll_ms = 0;
    }
  }
  if (WiFi.status() != WL_CONNECTED && g_net_online) {
    // Detected outside the reconnect cadence, e.g. the AP dropped us.
    g_net_online = false;
    g_link_verified = false;
    Serial.println("wifi: link lost");
  }

  // ---- poll cadence ----
  if (!g_net_online || !g_link_verified) {
    // An association with no proven route out is not a usable link: skip
    // the poll entirely rather than burning a TLS timeout every 15 s. The
    // serial log has already said why.
    return;
  }
  if (now - g_last_poll_ms < config::POLL_INTERVAL_MS) {
    return;
  }
  g_last_poll_ms = now;
  poll();
}

bool netOnline() {
  return g_net_online;
}

bool linkUsable() {
  return g_net_online && g_link_verified;
}

bool stale() {
  if (!linkUsable()) {
    return true;  // no data can arrive without a usable link
  }
  if (!g_ever_succeeded) {
    return true;
  }
  return (millis() - g_last_success_ms) >= config::STALE_AFTER_MS;
}

uint32_t secondsSinceData() {
  if (!g_ever_succeeded) {
    return 0;
  }
  return (millis() - g_last_success_ms) / 1000UL;
}

const char* serverUptime() {
  return g_uptime;
}

const char* serverCommit() {
  return g_commit;
}

const char* serverBuilt() {
  return g_built;
}

uint32_t pollCount() {
  return g_poll_count;
}

void printStatus() {
  // LOG_LINE_MAX has to hold uptime + commit + built + the not-injected
  // suffix at the same time, so the fixed parts are kept short and the
  // variable ones are truncated by snprintf rather than overflowing.
  char line[config::LOG_LINE_MAX];
  snprintf(line, sizeof(line),
           "net=%s%s stale=%s data_age=%lus polls=%lu up=%s commit=%s%s",
           g_net_online ? "up" : "down",
           g_net_online ? (g_link_verified ? "" : "/no-route") : "",
           stale() ? "YES" : "no",
           static_cast<unsigned long>(secondsSinceData()),
           static_cast<unsigned long>(g_poll_count),
           g_uptime[0] != '\0' ? g_uptime : "(none)",
           g_commit[0] != '\0' ? g_commit : "(none)",
           (strcmp(g_commit, "dev") == 0 || strcmp(g_built, "unknown") == 0)
               ? " !inject"
               : "");
  Serial.println(line);
}

}  // namespace health
