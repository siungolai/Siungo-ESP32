/**
 * health.cpp -- the state machine behind health.h
 *
 * Two documents, two clocks, two verdicts (docs/adr/0012):
 *
 *   snapshot.json  19 fields, polled every POLL_INTERVAL_MS (15 s),
 *                  stale after STALE_AFTER_MS (60 s)
 *   mailbox.json    5 fields, polled every MAILBOX_INTERVAL_MS (5 min),
 *                  stale after MAILBOX_STALE_AFTER_MS (8 min)
 *
 * They are never merged into one "is the server OK" answer. The mailbox
 * document is rewritten by a five-minute cron, so judging it by the server's
 * 60 s threshold would make a perfectly healthy mailbox look stale four
 * times out of five; and a mailbox cron that never ran must not make the
 * server screen claim the server is stale.
 *
 * Why the raw body is kept and parsed on demand instead of being unpacked
 * into 19 variables: the field set is a contract that will grow (ADR-0012
 * already went through one revision), and a table of 19 name->offset pairs
 * is exactly the thing that silently rots when a field is inserted in the
 * middle. jsonfield scans a 1 KiB body in microseconds, the screens ask for
 * at most a dozen values per repaint, and there is then no place where a
 * field can be forgotten.
 *
 * Network facts are tracked separately, because they fail independently:
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

#include <cmath>
#include <cctype>
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
// One timer pair per document. g_last_*_ms is "when the last attempt
// started" and gates the cadence; g_last_*_success_ms is "when the last
// usable body landed" and drives staleness. Keeping them apart is what makes
// "no immediate retry" (docs/adr/0004) fall out for free.
uint32_t g_last_poll_ms = 0;
uint32_t g_last_mailbox_poll_ms = 0;
uint32_t g_last_wifi_check_ms = 0;
uint32_t g_last_success_ms = 0;
uint32_t g_last_mailbox_success_ms = 0;
uint32_t g_poll_count = 0;
uint32_t g_mailbox_poll_count = 0;

// A first poll has never happened, so "seconds since data" would otherwise
// start from boot and look like a 10-minute-old server. Treated as stale
// until the first success, but reported distinctly on the serial log.
bool g_ever_succeeded = false;
bool g_mailbox_ever_succeeded = false;

// ---- the documents --------------------------------------------------------
// Kept verbatim, NUL-terminated, never larger than HTTP_RESPONSE_MAX.
// Empty until the first usable body lands; every accessor then naturally
// answers "no value" instead of needing a separate has-fetched flag.
char g_snapshot[config::HTTP_RESPONSE_MAX] = {0};
char g_mailbox[config::HTTP_RESPONSE_MAX] = {0};

// ---- new-mail edge detection ----------------------------------------------
// Double slot with a one-poll lag (docs/adr/0011). g_unread_* is the value
// from the most recent poll, g_prev_unread_* the one before it, so the
// comparison is always "did it go up since last time" and never "is it
// above the boot-time baseline".
//
// This is the decision ADR-0011 singles out as the easiest one to get wrong:
// seeding prevUnread with 0 would make every device that boots into a
// mailbox that already has mail fire a new-mail notification on the first
// poll. The lag makes that impossible by construction -- the first poll has
// nothing to compare against, so _known stays false and nothing fires.
long g_unread = 0;
long g_prev_unread = 0;
bool g_unread_known = false;
bool g_prev_unread_known = false;

// Latched rising edge. unreadRose() consumes it, so a screen that repaints
// several times between two mailbox polls still gets exactly one buzz.
bool g_unread_rise_pending = false;

// Rate limit for repeated failure logging: a 403 from nginx (key not
// deployed yet) would otherwise print a line every 15 s for as long as the
// misconfiguration lasts.
uint32_t g_last_fail_log_ms = 0;

// The first line of the most recent body that failed validation, for the
// log echo below. Kept at file scope so a failure deep inside fetchOnce()
// can explain itself without threading an extra parameter through.
char g_last_failure[64] = {0};

// Declared here, defined below: rememberFailure() sits between the state and
// the reporter, and both are used by every failure path in this file.
void logFailure(const char* why);

// ---- HTTP ----------------------------------------------------------------
// The client is created per request inside fetchOnce() -- see the note there
// about socket reuse. Nothing is cached at file scope.

// Remember a rejected body's first line, so the next logFailure() can show
// what actually arrived instead of only naming the failure mode.
void rememberFailure(const char* why, const char* body) {
  g_last_failure[0] = '\0';
  if (body != nullptr && body[0] != '\0') {
    size_t i = 0;
    while (i + 1 < sizeof(g_last_failure) && body[i] != '\0' &&
           body[i] != '\r' && body[i] != '\n') {
      g_last_failure[i] = body[i];
      ++i;
    }
    g_last_failure[i] = '\0';
  }
  logFailure(why);
}

// Every failure path reports through here, but only once per
// POLL_WARN_INTERVAL_MS. A poll that fails is normal and already visible on
// the screen as a rising age counter; a poll that keeps failing is the thing
// worth reading about, and it needs one line per minute, not four per minute.
void logFailure(const char* why) {
  const uint32_t now = millis();
  if (g_last_fail_log_ms != 0 &&
      now - g_last_fail_log_ms < config::POLL_WARN_INTERVAL_MS) {
    return;
  }
  g_last_fail_log_ms = now;
  Serial.print("[poll] ");
  Serial.println(why);
  // Echo the head of the rejected body: the messages above name the failure
  // mode, but telling "nginx answered 403" from "the captive portal
  // answered" from "the read was truncated mid-field" needs the payload, and
  // re-running the request by hand to see it is slow.
  if (g_last_failure[0] != '\0') {
    Serial.print("       body: ");
    Serial.println(g_last_failure);
  }
}

// Plain TCP probe of the data host. Not HTTPS: a TLS handshake would fail
// identically for "no route out", "DNS broken" and "certificate expired",
// which is exactly the ambiguity we are trying to remove. If this connect
// succeeds the L3 path to the host is fine and any later HTTPS failure is a
// certificate/DNS problem rather than a dead interface.
bool verifyLink() {
  WiFiClient probe;
  probe.setTimeout(5);
  const bool ok = probe.connect(config::DATA_HOST, config::DATA_PORT);
  if (!ok) {
    Serial.print("wifi: probe connect to ");
    Serial.print(config::DATA_HOST);
    Serial.print(":");
    Serial.print(config::DATA_PORT);
    Serial.print(" failed (errno ");
    Serial.print(errno);
    Serial.println(") -- interface has no route out");
    probe.stop();
    return false;
  }
  probe.stop();
  return true;
}

// Is this body a complete flat JSON object? A truncated read (the 1 KiB cap,
// a dropped connection) is accepted by neither document's contract: ADR-0012
// makes a document either complete or absent. Parsing it would freeze every
// field past the cut as null until the next successful poll, and a null on
// screen means "the collector could not measure", which is a lie.
//
// Cheap on purpose: brace/bracket balance with a string literal skip, no
// recursion, no numbers, no allocation. Anything that fails here also fails
// jsonfield's own value scanner, so this is a guard, not a second parser.
bool looksComplete(const char* body) {
  if (body == nullptr) {
    return false;
  }
  const char* p = body;
  while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
    ++p;
  }
  if (*p != '{') {
    return false;
  }
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (; *p != '\0'; ++p) {
    const char c = *p;
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth == 0) {
        // The object closed; only trailing whitespace may follow, so a body
        // holding two concatenated documents (a proxy quirk) is rejected
        // rather than half-parsed.
        ++p;
        while (*p != '\0' && isspace(static_cast<unsigned char>(*p))) {
          ++p;
        }
        return *p == '\0';
      }
      if (depth < 0) {
        return false;
      }
    }
  }
  return false;  // ran out of body before the object closed
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

// Fetch one document into a caller-provided static buffer. Returns true when
// bytes arrived; whether they are the right document is the caller's job.
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
bool fetchOnce(const char* path, char* body, size_t cap) {
  WiFiClientSecure client;
  client.setTimeout(config::HTTP_TIMEOUT_MS / 1000);
  client.setInsecure();

  HTTPClient http;
  String url = String("https://") + config::DATA_HOST + path;
  if (!http.begin(client, url)) {
    rememberFailure("http.begin() failed", nullptr);
    return false;
  }
  http.setTimeout(config::HTTP_TIMEOUT_MS);
  // M1b sends the shared key; an empty MONITOR_KEY (pre-deployment) simply
  // omits the header, which nginx answers with 403 -- the honest signal that
  // the server side is not switched over yet (docs/adr/0012: deploy order is
  // not negotiable).
  if (MONITOR_KEY[0] != '\0') {
    http.addHeader(config::MONITOR_KEY_HEADER, MONITOR_KEY);
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      rememberFailure("HTTP transport error (no response)", nullptr);
    } else {
      char msg[32];
      snprintf(msg, sizeof(msg), "HTTP %d", code);
      rememberFailure(msg, nullptr);
    }
    http.end();
    return false;
  }

  // Read at most cap-1 bytes so the buffer stays NUL-terminated.
  //
  // De-chunking is HTTPClient's job (it hands back the payload, not the
  // transfer-encoded bytes), so what lands here is exactly the body. What is
  // NOT handled here is a body that arrives as a full HTTP message: if the
  // headers are in the stream the framing is not what this reader expects,
  // and parsing the whole thing as JSON would reject every document.
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
    rememberFailure("empty body", body);
    return false;
  }
  // The chunked framing is the server's business, not ours -- but the
  // response must not still be a full HTTP message. If the headers are in
  // the stream the framing is not what this reader expects, and parsing the
  // whole thing as JSON would silently reject every document.
  if (strstr(body, "\r\n\r\n") != nullptr) {
    rememberFailure("response body still contains HTTP headers", body);
    return false;
  }
  return true;
}

// One poll of snapshot.json. Two gates, in this order:
//
//   1. looksComplete() -- the body is a whole flat JSON object and nothing
//      else. A truncated read would otherwise be adopted as the last known
//      good state and every field past the cut would read as null forever,
//      and a null on screen means "the collector could not measure", which
//      would be a lie (ADR-0012: a document is either complete or absent).
//   2. jsonfield::hasKey(body, "generated_at") -- the body is the document
//      and not something else. This is the gate that matters for the real
//      failure mode measured on the bench 2026-10-17: with the /status/
//      location missing, nginx falls through to the SPA catch-all and
//      answers 200 with index.html, which is neither an error page nor a
//      document and would otherwise look like a working endpoint.
void pollSnapshot() {
  static char body[config::HTTP_RESPONSE_MAX];

  if (!fetchOnce(config::DATA_PATH, body, sizeof(body))) {
    return;  // failure: no state change beyond "not fresh" (no immediate retry)
  }

  // Accept the body only when it parses as a complete flat object with no
  // trailing garbage. ADR-0012 says a document is either complete or absent,
  // so "starts with generated_at" is not enough: a truncated read would be
  // adopted as the last known good state and every field past the cut would
  // read as null forever. Rejecting instead means the previous body stays in
  // place and the screen keeps showing numbers that were at least complete.
  if (!looksComplete(body)) {
    rememberFailure("snapshot body is not a complete flat JSON object", body);
    return;
  }
  // The nginx location is inserted BEFORE the SPA fallback
  // (docs/adr/0008), so a request that reaches the site's catch-all returns
  // index.html with 200 -- a body that is neither the document nor an error
  // page, and the one failure that would otherwise look like a working
  // endpoint. Asking for the document's own first field is what separates
  // them. Note the order: looksComplete() rejects the SPA page first (it is
  // not JSON at all), and this gate catches the case where an HTML page
  // happens to contain a JSON-looking script tag.
  if (!jsonfield::hasKey(body, "generated_at")) {
    rememberFailure("snapshot body without generated_at; not counted as data",
                    body);
    return;
  }

  memcpy(g_snapshot, body, sizeof(g_snapshot));
  g_last_success_ms = millis();
  g_poll_count++;
  g_ever_succeeded = true;

  Serial.print("[poll] snapshot ok polls=");
  Serial.println(g_poll_count);
}

// One poll of mailbox.json. All five fields are scalars, so they are
// unpacked straight into the edge-detection slots; the raw body is still
// kept because mailboxAvailable() and friends are read on every repaint and
// re-scanning 1 KiB is cheaper than keeping a second copy of the truth in
// sync.
void pollMailbox() {
  static char body[config::HTTP_RESPONSE_MAX];

  if (!fetchOnce(config::MAILBOX_PATH, body, sizeof(body))) {
    return;
  }

  // available is the field that says this document is the mailbox document
  // (0/1 per ADR-0012). Without it the body is not ours. Note that
  // available=0 is a legitimate value (no credentials configured), so the
  // test is "is the key there with a number", not "is it 1".
  double available = 0.0;
  if (!jsonfield::number(body, "available", &available)) {
    rememberFailure("mailbox body without available; not counted as data",
                    body);
    return;
  }
  if (!looksComplete(body)) {
    rememberFailure("mailbox body is not a complete flat JSON object", body);
    return;
  }

  memcpy(g_mailbox, body, sizeof(g_mailbox));
  g_last_mailbox_success_ms = millis();
  g_mailbox_poll_count++;
  g_mailbox_ever_succeeded = true;

  // Shift the edge-detection slots: today's value becomes tomorrow's
  // previous value, and an unread of null is recorded as "unknown" rather
  // than as a number, because a partial total (one IMAP account failed) is
  // not evidence that no mail arrived (docs/adr/0012 rule 6).
  g_prev_unread = g_unread;
  g_prev_unread_known = g_unread_known;

  double unread = 0.0;
  if (jsonfield::number(body, "unread", &unread)) {
    g_unread = static_cast<long>(unread);
    g_unread_known = true;
  } else {
    g_unread_known = false;
  }

  // Rising edge, evaluated exactly once per poll. Both sides must be known:
  // unknown -> 5 is not proof that anything arrived, and 5 -> unknown is not
  // proof that anything was read.
  if (g_unread_known && g_prev_unread_known && g_unread > g_prev_unread) {
    g_unread_rise_pending = true;
  }

  Serial.print("[poll] mailbox ok available=");
  Serial.print(available > 0.5 ? 1 : 0);
  Serial.print(" unread=");
  if (g_unread_known) {
    Serial.print(g_unread);
  } else {
    Serial.print("null");
  }
  Serial.print(" polls=");
  Serial.println(g_mailbox_poll_count);
}

}  // namespace

void begin() {
  g_net_online = false;
  g_link_verified = false;
  g_last_poll_ms = millis();
  g_last_mailbox_poll_ms = millis();
  g_last_wifi_check_ms = millis();
  g_last_success_ms = 0;
  g_last_mailbox_success_ms = 0;
  g_poll_count = 0;
  g_mailbox_poll_count = 0;
  g_ever_succeeded = false;
  g_mailbox_ever_succeeded = false;
  g_snapshot[0] = '\0';
  g_mailbox[0] = '\0';
  g_unread = 0;
  g_prev_unread = 0;
  g_unread_known = false;
  g_prev_unread_known = false;
  g_unread_rise_pending = false;
  g_last_fail_log_ms = 0;
  g_last_failure[0] = '\0';
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
      // interval: the user just watched the network come back. Both
      // documents, because a five-minute-old mailbox is as stale as it gets
      // after a reconnect.
      g_last_poll_ms = 0;
      g_last_mailbox_poll_ms = 0;
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

  // At most one fetch per update(). Both requests are blocking and bounded
  // by HTTP_TIMEOUT_MS, and the loop calls update() continuously, so this
  // costs nothing in latency while keeping a single iteration from stalling
  // the buttons and the 1 s local repaint for two TLS round trips.
  //
  // Mailbox first when both come due: it is the rarer document, and if the
  // snapshot fetch were allowed to consume the iteration the mailbox could
  // be starved on a slow link for as long as the snapshot keeps failing.
  if (now - g_last_mailbox_poll_ms >= config::MAILBOX_INTERVAL_MS) {
    g_last_mailbox_poll_ms = now;
    pollMailbox();
  } else if (now - g_last_poll_ms >= config::POLL_INTERVAL_MS) {
    g_last_poll_ms = now;
    pollSnapshot();
  }
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

bool hasNumber(const char* key) {
  double v = 0.0;
  return jsonfield::number(g_snapshot, key, &v);
}

double number(const char* key) {
  double v = 0.0;
  if (!jsonfield::number(g_snapshot, key, &v)) {
    return NAN;
  }
  return v;
}

const char* string(const char* key) {
  // A null/absent value has to be distinguishable from an empty string, and
  // the extractor already answers "" for both. The screens render "" and
  // "no value" identically ("-"), so one buffer serves both.
  //
  // The buffer is static, not a stack temporary: the caller's return value
  // is used in the same snprintf() call that consumes it, so a stack buffer
  // would be fine -- but two calls in one expression ("a" then "b") would
  // then alias. The screens never do that, and a static keeps that mistake
  // from becoming a silent corruption of one field by another.
  static char out[48];
  jsonfield::field(g_snapshot, key, out, sizeof(out));
  return out;
}

bool mailboxAvailable() {
  double v = 0.0;
  return jsonfield::number(g_mailbox, "available", &v) && v > 0.5;
}

bool mailboxUnreadKnown() {
  double v = 0.0;
  return jsonfield::number(g_mailbox, "unread", &v);
}

long mailboxUnread() {
  if (!g_unread_known) {
    return 0;
  }
  return g_unread;
}

unsigned mailboxAccountCount() {
  double v = 0.0;
  if (!jsonfield::number(g_mailbox, "account_count", &v)) {
    return 0;
  }
  return static_cast<unsigned>(v);
}

unsigned mailboxFailedCount() {
  double v = 0.0;
  if (!jsonfield::number(g_mailbox, "failed_count", &v)) {
    return 0;
  }
  return static_cast<unsigned>(v);
}

bool mailboxStale() {
  if (!linkUsable()) {
    return true;
  }
  if (!g_mailbox_ever_succeeded) {
    return true;
  }
  return (millis() - g_last_mailbox_success_ms) >=
         config::MAILBOX_STALE_AFTER_MS;
}

uint32_t mailboxSecondsSinceData() {
  if (!g_mailbox_ever_succeeded) {
    return 0;
  }
  return (millis() - g_last_mailbox_success_ms) / 1000UL;
}

bool unreadRose() {
  // Consume, don't just report: main.cpp asks once per loop iteration and a
  // latch is the only thing that keeps one rising edge from becoming forty
  // buzzes while the mailbox screen repaints.
  if (!g_unread_rise_pending) {
    return false;
  }
  g_unread_rise_pending = false;
  return true;
}

uint32_t pollCount() {
  return g_poll_count;
}

uint32_t mailboxPollCount() {
  return g_mailbox_poll_count;
}

void printStatus() {
  // LOG_LINE_MAX has to hold both documents' summaries at once, so the fixed
  // parts are kept short and the variable ones are truncated by snprintf
  // rather than overflowing.
  char line[config::LOG_LINE_MAX];
  snprintf(line, sizeof(line),
           "net=%s%s snap=%s age=%lus polls=%lu mail=%s age=%lus polls=%lu "
           "unread=%s",
           g_net_online ? "up" : "down",
           g_net_online ? (g_link_verified ? "" : "/no-route") : "",
           stale() ? "STALE" : "fresh",
           static_cast<unsigned long>(secondsSinceData()),
           static_cast<unsigned long>(g_poll_count),
           mailboxStale() ? "STALE" : "fresh",
           static_cast<unsigned long>(mailboxSecondsSinceData()),
           static_cast<unsigned long>(g_mailbox_poll_count),
           mailboxUnreadKnown() ? "known" : "null");
  Serial.println(line);
}

}  // namespace health
