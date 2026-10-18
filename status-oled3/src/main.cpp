/**
 * main.cpp -- status-oled3 firmware, M0 (hardware bringup) + M1a (data path)
 * + M1b (two documents, full screens)
 *
 * M0 scope (see README.md milestone table): the PCA9548A + three SSD1306
 * chain, the two buttons, and the buzzer. Everything on screen here is
 * either hardware state or a placeholder, so that a "broken" screen can be
 * told apart from a "no data yet" screen by eye.
 *
 * M1a scope (docs/adr/0009): the device-side network path only -- WiFi,
 * HTTPS GET, JSON field extraction, the 15 s poll cadence, the 60 s
 * staleness verdict, and the two buzzer edges that data can produce (WiFi
 * lost, data stale).
 *
 * M1b scope (docs/adr/0012): the server screen becomes two pages of real
 * snapshot.json fields and the mailbox screen becomes real mailbox.json
 * values, plus the third buzzer edge (new mail). The server side is deployed
 * (README.md: M1b 已上线 2026-10-17); the 403 path stays in the code because
 * it is the correct behaviour for a key that is not switched on yet, not a
 * placeholder.
 *
 * Structure: five independent cadences sharing one loop().
 *   - buttons: drained every iteration (a missed press is a lost press)
 *   - network: 15 s snapshot poll, 5 min mailbox poll, 5 s link re-check
 *   - ESP32 status screen: 1 s (uptime changes every second)
 *   - the other two screens: only when a poll lands or focus moves
 * The screen layer owns the dirty flag, so an iteration with nothing to do
 * costs no I2C traffic at all.
 *
 * "Only when a poll lands" is enforced in handleEdges(): the two data screens
 * are invalidated on the TRANSITION into each state, not while it persists.
 * An earlier version invalidated them on every loop() while the link was up,
 * which turned the data screens into a busy-redraw loop and made the claim
 * above false.
 */
#include <Arduino.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
// strlen() is used by the boot report; it comes in through Arduino.h on this
// platform, but include it explicitly rather than rely on that.
#include <cstring>

#include "button.h"
#include "buzzer.h"
#include "config.h"
#include "health.h"
#include "pca9548a.h"
#include "screen.h"

namespace {

// Elapsed time in ms, correct across the millis() wrap (49.7 days). The
// subtraction is done in uint32_t, so a wrapped "now" still yields the true
// small positive difference. Every cadence in this file compares against a
// threshold, never against 0, which is what makes the unsigned form safe.
//
// It lives here rather than at file scope so that it stays private to this
// translation unit: main.cpp is the only file that schedules anything, and
// duplicating a wrap-safe comparison helper in a header would invite the two
// copies to drift apart (health.cpp has its own for the same reason).
inline uint32_t elapsedSince(uint32_t now, uint32_t start) {
  return now - start;
}

// ---- focus / page state (the M0 interaction model) ------------------------
// Which screen currently answers to the buttons. Power-on default is the
// ESP32 status screen (docs/adr/0001).
screen::ScreenId g_focus = screen::SCREEN_LOCAL;

// Page count of each screen (docs/adr/0001):
//   local   -- one page, six rows of local facts fit without scrolling
//   server  -- two pages: (1) cpu/mem/load, (2) disk/net/services
//   mailbox -- one page: five rows fit, and ADR-0001 explicitly says the
//              mailbox does not need paging
// The counts are fixed by the ADR, not derived from what happens to be
// non-null today: a field that is null must still occupy its row, or the
// layout would shift under the user when one collector breaks.
//
// This is the second copy of the truth: screen::setPageCount() stores the same
// numbers inside the screen layer, and screen::pageCount() is the read side.
// main.cpp pushes them once in setup() and never reads them back; the array
// exists so the push is a table instead of three calls.
uint8_t g_page_count[screen::SCREEN_COUNT] = {1, 2, 1};

// ---- refresh timing -------------------------------------------------------
uint32_t g_last_local_ms = 0;
uint32_t g_last_rescan_ms = 0;
uint32_t g_last_netlog_ms = 0;

// ---- buzzer edges (docs/adr/0011) -----------------------------------------
// The three "previous" values that turn a state into an event. The initial
// values are chosen so that booting never fires a pattern by itself:
//   prevNetOnline = true  -> the first "down" is a real transition
//   prevStale     = false -> the first "stale" is a real transition
// The new-mail edge does not need a prev value here: health.cpp keeps its
// own double-slot history, so a device that boots into a mailbox that
// already has mail does not announce it (ADR-0011's own warning about the
// one decision that is easiest to get wrong).
bool g_prev_net_online = true;
bool g_prev_link_usable = false;
bool g_prev_stale = false;
// Rising-edge detectors for the two poll counters, so the data screens are
// invalidated exactly when new data lands (see handleEdges).
uint32_t g_prev_poll_count = 0;
uint32_t g_prev_mailbox_poll_count = 0;

// A screen unplugged and replugged should come back on its own.
constexpr uint32_t kRescanIntervalMs = config::RESCAN_INTERVAL_MS;

// Serial log cadence for the network line. One line per minute keeps the log
// readable during a long uptime while still showing the trend.
constexpr uint32_t kNetLogIntervalMs = 60000;

// ---- small formatting helpers --------------------------------------------
// 128 px at 6 px per character = 21 columns. Everything is sized to fit in
// 32-byte buffers with room to spare.

// Seconds -> "12d 03:45:06" style uptime.
void formatUptime(char* out, size_t n, uint32_t seconds) {
  const uint32_t days = seconds / 86400UL;
  const uint32_t hours = (seconds / 3600UL) % 24UL;
  const uint32_t mins = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;
  snprintf(out, n, "%lud %02lu:%02lu:%02lu",
           static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(mins),
           static_cast<unsigned long>(secs));
}

// kB -> "1.6G" / "421M" / "0.9G" style. The snapshot sends kB (ADR-0012),
// and a 16-digit kB number does not fit next to a label; a human-readable
// size does. Rounding is truncation on purpose -- a used value that reads
// slightly low is honest, one that rounds up claims memory that is still
// free.
void formatKb(char* out, size_t n, double kb) {
  if (!(kb >= 0.0)) {  // also catches NAN
    snprintf(out, n, "%s", "-");
    return;
  }
  if (kb >= 1024.0 * 1024.0) {
    snprintf(out, n, "%.1fG", kb / (1024.0 * 1024.0));
  } else if (kb >= 1024.0) {
    snprintf(out, n, "%luM", static_cast<unsigned long>(kb / 1024.0));
  } else {
    snprintf(out, n, "%luK", static_cast<unsigned long>(kb));
  }
}

// "used/total" as two formatKb() halves in one buffer: the memory and disk
// rows. Written as one snprintf instead of formatKb + writing a '/' into the
// buffer + a second formatKb into the remainder -- that form is only safe
// because formatKb is short, and it silently assumes the caller knows how
// much room is left. snprintf states the bound.
void formatPair(char* out, size_t n, double a, double b) {
  char left[16];
  char right[16];
  formatKb(left, sizeof(left), a);
  formatKb(right, sizeof(right), b);
  snprintf(out, n, "%s/%s", left, right);
}

// A percentage with an unknown denominator is not a number (ADR-0012): a
// null on either side renders "-" rather than 0, which would read as
// "nothing used".
void formatPercent(char* out, size_t n, double used, double total) {
  if (!(used >= 0.0) || !(total > 0.0)) {
    snprintf(out, n, "%s", "-");
    return;
  }
  snprintf(out, n, "%lu%%", static_cast<unsigned long>((used * 100.0) / total));
}

// One snapshot value as a row value. hasNumber() first, so null and absent
// both land on "-" instead of on a 0 that would look like a measurement.
// Numbers are printed as integers: every field in ADR-0012 is an integer
// count (seconds, kB, bytes, a 0/1 flag, days), so a decimal point would
// only ever be noise.
bool kvNumber(screen::ScreenId id, uint8_t row, const char* key, const char* label,
              const char* unit) {
  if (!health::hasNumber(key)) {
    screen::printKV(id, row, label, "-");
    return false;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu%s",
           static_cast<unsigned long>(health::number(key)), unit);
  screen::printKV(id, row, label, buf);
  return true;
}

// Same, but for the fields that stay strings (net_if, generated_at).
void kvString(screen::ScreenId id, uint8_t row, const char* key, const char* label) {
  const char* v = health::string(key);
  screen::printKV(id, row, label, (v != nullptr && v[0] != '\0') ? v : "-");
}

const char* screenTitle(screen::ScreenId id) {
  switch (id) {
    case screen::SCREEN_LOCAL:
      return "ESP32";
    case screen::SCREEN_SERVER:
      return "SERVER";
    case screen::SCREEN_MAILBOX:
      return "MAILBOX";
    default:
      return "?";
  }
}

// ---- the three screens ----------------------------------------------------

void drawLocal() {
  if (!screen::online(screen::SCREEN_LOCAL)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_LOCAL, "ESP32 STATUS",
                       g_focus == screen::SCREEN_LOCAL);

  const uint32_t now_s = millis() / 1000UL;
  char buf[32];

  formatUptime(buf, sizeof(buf), now_s);
  screen::printKV(screen::SCREEN_LOCAL, 1, "up", buf);

  // WiFi: real since M1a. The RSSI only while the link is proven usable --
  // "associated with no route out" shows as "no route" instead of a healthy
  // signal number that would suggest everything is fine.
  if (!health::netOnline()) {
    snprintf(buf, sizeof(buf), "%s", "connecting");
  } else if (!health::linkUsable()) {
    snprintf(buf, sizeof(buf), "%s", "no route out");
  } else {
    snprintf(buf, sizeof(buf), "%d dBm", static_cast<int>(WiFi.RSSI()));
  }
  screen::printKV(screen::SCREEN_LOCAL, 2, "wifi", buf);

  // Heap: the number that catches a slow memory leak before it bites.
  snprintf(buf, sizeof(buf), "%lu",
           static_cast<unsigned long>(ESP.getFreeHeap()));
  screen::printKV(screen::SCREEN_LOCAL, 3, "heap", buf);

  // Screen presence is the M0 headline result, so it gets its own line
  // rather than being buried in the serial log. Four bytes: three flags plus
  // the terminator. (Four, not five -- an earlier version sized it 5 and left
  // the last byte unused, which reads as a miscount.)
  char online_flags[4];
  online_flags[0] = screen::online(screen::SCREEN_LOCAL) ? '1' : '0';
  online_flags[1] = screen::online(screen::SCREEN_SERVER) ? '1' : '0';
  online_flags[2] = screen::online(screen::SCREEN_MAILBOX) ? '1' : '0';
  online_flags[3] = '\0';
  screen::printKV(screen::SCREEN_LOCAL, 4, "scr", online_flags);

  // The full dotted quad, not the last octet: an earlier revision printed
  // only the last octet on the assumption that a full quad does not fit. It
  // does -- "ip=192.168.1.100" is 18 characters x 6 px = 108 px of the 128 px
  // width -- and the full address is what you need to reach the device, so
  // the octet-only version was the wrong trade.
  if (health::netOnline()) {
    const IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    screen::printKV(screen::SCREEN_LOCAL, 5, "ip", buf);
  } else {
    screen::printKV(screen::SCREEN_LOCAL, 5, "ip", "-");
  }

  screen::printKV(screen::SCREEN_LOCAL, 6, "btn", "A=page B=focus");
}

// Server screen page 1: CPU, memory, swap, load.
void drawServerPage1() {
  // CPU: the one field that must be shown as a bare number. cpu_pct is
  // already a percentage (ADR-0012), so "%" is the only unit.
  kvNumber(screen::SCREEN_SERVER, 1, "cpu_pct", "cpu", "%");

  // Memory as a used percentage AND as sizes: the percentage is the number
  // a human reads, the sizes are what make it auditable. Both sides of the
  // percentage come from the same document, so a null on either side is a
  // broken collector rather than a rounding problem, and "-" says exactly
  // that.
  const double mem_used = health::number("mem_used_kb");
  const double mem_total = health::number("mem_total_kb");
  char buf[32];
  formatPercent(buf, sizeof(buf), mem_used, mem_total);
  screen::printKV(screen::SCREEN_SERVER, 2, "mem", buf);

  char sizes[32];
  // "used/total" in one buffer. Written as a single bounded snprintf rather
  // than formatKb + manual '/' + second formatKb: the previous form wrote
  // sizes[strlen(sizes)] = '/' with no check that anything was left, which is
  // only safe because formatKb happens to be short. snprintf makes the bound
  // explicit and cannot be broken by a future change to formatKb.
  formatPair(sizes, sizeof(sizes), mem_used, mem_total);
  screen::printKV(screen::SCREEN_SERVER, 3, "sz", sizes);

  // Swap: shown only as a percentage, because a swap partition is either
  // empty (0%, the healthy case) or a problem, and the absolute size adds
  // nothing to that decision.
  const double swap_used = health::number("swap_used_kb");
  const double swap_total = health::number("swap_total_kb");
  formatPercent(buf, sizeof(buf), swap_used, swap_total);
  screen::printKV(screen::SCREEN_SERVER, 4, "swap", buf);

  // Load: load1_x100 is loadavg * 100 (ADR-0012), so it needs the decimal
  // point put back to be readable as a load average.
  if (health::hasNumber("load1_x100")) {
    const double load = health::number("load1_x100") / 100.0;
    snprintf(buf, sizeof(buf), "%.2f", load);
    screen::printKV(screen::SCREEN_SERVER, 5, "load1", buf);
  } else {
    screen::printKV(screen::SCREEN_SERVER, 5, "load1", "-");
  }
}

// Server screen page 2: disk, network, service probes.
void drawServerPage2() {
  const double disk_used = health::number("disk_used_kb");
  const double disk_total = health::number("disk_total_kb");
  char buf[32];
  formatPercent(buf, sizeof(buf), disk_used, disk_total);
  screen::printKV(screen::SCREEN_SERVER, 1, "disk", buf);

  char sizes[32];
  // See drawServerPage1(): one bounded snprintf, no manual splicing.
  formatPair(sizes, sizeof(sizes), disk_used, disk_total);
  screen::printKV(screen::SCREEN_SERVER, 2, "sz", sizes);

  // Interface name only: the byte counters stay strings because a 64-bit
  // counter does not survive a double (ADR-0012 note), and the screen has
  // no room for "eth0 976056208" anyway.
  kvString(screen::SCREEN_SERVER, 3, "net_if", "if");

  // Service probes: the whole point of these three fields is the 0/1, so
  // they are rendered as words. A null means the probe could not run, which
  // is not "down" -- the collector could not reach the unit, the service is
  // not necessarily stopped.
  //
  // Layout note (2026-10-17): these used to occupy rows 4, 5 and 6. Row 6 is
  // also where drawServer() draws the page indicator, and the indicator runs
  // AFTER this function. Because drawText5x7() ORs pixels and never erases,
  // the indicator did not replace "upl=up" -- the two strings were merged into
  // one unreadable smear on the second server page, every single frame.
  // Row 7 (freshness) is out of reach for the same reason in reverse, so rows
  // 4 and 5 are all that is left here. web and api therefore stay, and the
  // upload probe moves into the page indicator on row 6 (see drawServer()):
  // "pg=2/2 upl=up" is one string, drawn once.
  const char* probe_label[2] = {"web", "api"};
  const char* probe_key[2] = {"svc_web", "svc_api"};
  for (uint8_t i = 0; i < 2; ++i) {
    if (!health::hasNumber(probe_key[i])) {
      screen::printKV(screen::SCREEN_SERVER, static_cast<uint8_t>(4 + i),
                      probe_label[i], "-");
      continue;
    }
    const double v = health::number(probe_key[i]);
    screen::printKV(screen::SCREEN_SERVER, static_cast<uint8_t>(4 + i),
                    probe_label[i], v > 0.5 ? "up" : "down");
  }
}

void drawServer() {
  if (!screen::online(screen::SCREEN_SERVER)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_SERVER, "SERVER",
                       g_focus == screen::SCREEN_SERVER);

  // Freshness first, on both pages: it is the only field that says whether
  // the numbers below it are current. "no data yet" is deliberately distinct
  // from "stale" -- the first means the device has never had a document
  // (expected before the server side is deployed), the second means it had
  // one and stopped getting them (a real failure).
  if (!health::netOnline()) {
    screen::printKV(screen::SCREEN_SERVER, 7, "data", "wifi down");
  } else if (!health::linkUsable()) {
    screen::printKV(screen::SCREEN_SERVER, 7, "data", "no route out");
  } else if (health::stale()) {
    char buf[32];
    const uint32_t age = health::secondsSinceData();
    if (age == 0) {
      screen::printKV(screen::SCREEN_SERVER, 7, "data", "no data yet");
    } else {
      snprintf(buf, sizeof(buf), "stale %lus",
               static_cast<unsigned long>(age));
      screen::printKV(screen::SCREEN_SERVER, 7, "data", buf);
    }
  } else {
    screen::printKV(screen::SCREEN_SERVER, 7, "data", "fresh");
  }

  if (screen::pageIndex(screen::SCREEN_SERVER) == 0) {
    drawServerPage1();
  } else {
    drawServerPage2();
  }

  // Page indicator, only when there is more than one page: with one page it
  // would be a permanent, meaningless decoration.
  //
  // On page 2 this row also carries the upload probe. Both want row 6 and
  // only one string fits, so they are composed into one line. The alternative
  // -- drawing a probe and then the indicator over it -- does not overwrite:
  // drawText5x7() ORs pixels, so the overlap rendered as both glyph sets at
  // once (the 2026-10-17 ghosting report).
  if (screen::pageCount(screen::SCREEN_SERVER) > 1) {
    char buf[32];
    const uint8_t page_no =
        static_cast<uint8_t>(screen::pageIndex(screen::SCREEN_SERVER) + 1);
    const uint8_t page_n = screen::pageCount(screen::SCREEN_SERVER);
    if (page_no == 2 && health::hasNumber("svc_upload")) {
      snprintf(buf, sizeof(buf), "%u/%u upl=%s", static_cast<unsigned>(page_no),
               static_cast<unsigned>(page_n),
               health::number("svc_upload") > 0.5 ? "up" : "down");
    } else {
      snprintf(buf, sizeof(buf), "%u/%u A=page",
               static_cast<unsigned>(page_no),
               static_cast<unsigned>(page_n));
    }
    screen::printKV(screen::SCREEN_SERVER, 6, "pg", buf);
  }
}

void drawMailbox() {
  if (!screen::online(screen::SCREEN_MAILBOX)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_MAILBOX, "MAILBOX",
                       g_focus == screen::SCREEN_MAILBOX);

  char buf[32];

  // The headline number. Three states, not two, because "no mail" and "we
  // cannot know" are different situations:
  //   n          -- a real count from the server
  //   -          -- available but unread is null: at least one IMAP account
  //                 failed, so the total would be a lie (ADR-0012 rule 6)
  //   n/a        -- available=0: no credentials configured on the server at
  //                 all, which is a setup state, not an empty inbox
  if (!health::mailboxAvailable()) {
    screen::printKV(screen::SCREEN_MAILBOX, 1, "unread", "n/a");
  } else if (!health::mailboxUnreadKnown()) {
    screen::printKV(screen::SCREEN_MAILBOX, 1, "unread", "-");
  } else {
    snprintf(buf, sizeof(buf), "%ld", health::mailboxUnread());
    screen::printKV(screen::SCREEN_MAILBOX, 1, "unread", buf);
  }

  // Accounts and failures. failed_count > 0 is the only reason unread can
  // be null, so showing it next to the headline is what makes the "-"
  // above self-explanatory.
  if (health::mailboxAvailable()) {
    snprintf(buf, sizeof(buf), "%u", health::mailboxAccountCount());
    screen::printKV(screen::SCREEN_MAILBOX, 2, "accts", buf);
    const unsigned failed = health::mailboxFailedCount();
    snprintf(buf, sizeof(buf), "%u", failed);
    screen::printKV(screen::SCREEN_MAILBOX, 3, "failed", buf);
  } else {
    screen::printKV(screen::SCREEN_MAILBOX, 2, "accts", "-");
    screen::printKV(screen::SCREEN_MAILBOX, 3, "failed", "-");
  }

  // Freshness, with the mailbox's own threshold: a five-minute cron is
  // healthy at four minutes old, so "fresh" here means something different
  // from what it means on the server screen.
  if (!health::netOnline()) {
    screen::printKV(screen::SCREEN_MAILBOX, 4, "data", "wifi down");
  } else if (!health::linkUsable()) {
    screen::printKV(screen::SCREEN_MAILBOX, 4, "data", "no route out");
  } else if (health::mailboxStale()) {
    const uint32_t age = health::mailboxSecondsSinceData();
    if (age == 0) {
      screen::printKV(screen::SCREEN_MAILBOX, 4, "data", "no data yet");
    } else {
      // The mailbox cadence is minutes, so the useful unit is minutes:
      // "stale 7m" says more than "stale 421s".
      snprintf(buf, sizeof(buf), "stale %lum",
               static_cast<unsigned long>(age / 60UL));
      screen::printKV(screen::SCREEN_MAILBOX, 4, "data", buf);
    }
  } else {
    screen::printKV(screen::SCREEN_MAILBOX, 4, "data", "fresh");
  }

  snprintf(buf, sizeof(buf), "%lu",
           static_cast<unsigned long>(health::mailboxPollCount()));
  screen::printKV(screen::SCREEN_MAILBOX, 5, "polls", buf);

  // ADR-0001 records that the mailbox screen has no second page, so button A
  // here is a permanent no-op; the on-screen hint keeps that from looking
  // like a dead button.
  screen::printKV(screen::SCREEN_MAILBOX, 6, "pg", "1 page, B=focus");
}

// ---- screen / serial output -------------------------------------------------
// The panel only carries one screen at a time in the single-panel topology,
// so nothing here needs a "what am I showing" lock. What it does need is a
// lock between "redraw what changed" and "the user asked for a different
// screen": if the 1 s local repaint ran after a focus switch, the new screen
// would be overwritten before the user saw it.
//
// Every screen is drawn through here, and this is the only place a screen is
// drawn, so the rule holds by construction rather than by remembering it.
void renderScreen(screen::ScreenId id) {
  // A focus switch queues a repaint of all three; the ones that are not
  // showable would only burn I2C time, and the panel would end up showing
  // whichever screen happened to be drawn last.
  if (!screen::showable(id)) {
    return;
  }
  switch (id) {
    case screen::SCREEN_LOCAL:
      drawLocal();
      break;
    case screen::SCREEN_SERVER:
      drawServer();
      break;
    case screen::SCREEN_MAILBOX:
      drawMailbox();
      break;
    default:
      break;
  }
  screen::flush(id);
}

// ---- button handling ------------------------------------------------------

void onButtonPage() {
  // Page flip only ever affects the focused screen (docs/adr/0001). When
  // the focused screen has one page there is nothing to flip, so it gets the
  // "empty operation" click instead of silence -- otherwise the user cannot
  // tell a working button from a dead one.
  //
  // A successful flip gets the same short click. Without it the two buttons
  // answer inconsistently: A only sounds when it does nothing, B never
  // sounds at all. Both buttons now acknowledge every accepted press
  // (docs/adr/0003, "button feedback").
  if (screen::pageCount(g_focus) <= 1) {
    buzzer::patternButtonEmpty();
    Serial.println("page: focused screen has 1 page, nothing to flip");
    return;
  }
  screen::nextPage(g_focus);
  // The page flipped on the focused screen; in the single-panel topology that
  // is the one on screen, so nothing else needs invalidating.
  screen::setFocused(g_focus);
  buzzer::patternButtonEmpty();
  Serial.print("page: ");
  Serial.println(screenTitle(g_focus));
}

void onButtonFocus() {
  // Cycle focus LOCAL -> SERVER -> MAILBOX -> LOCAL. A screen that is
  // offline is still focusable: focus is about which screen the buttons
  // control, not which screen happens to be lit, and skipping offline
  // screens would make the cycle length depend on hardware state.
  g_focus = static_cast<screen::ScreenId>(
      (static_cast<uint8_t>(g_focus) + 1) % screen::SCREEN_COUNT);

  // The new focus has to reach the panel. With three panels every title bar
  // carries the focus marker, so all three are redrawn; with one panel only
  // the newly focused screen is showable and takes over the panel.
  screen::setFocused(g_focus);
  screen::invalidate(screen::SCREEN_LOCAL);
  screen::invalidate(screen::SCREEN_SERVER);
  screen::invalidate(screen::SCREEN_MAILBOX);
  // Acknowledge the press the same way button A does, so both buttons answer
  // every accepted press with a short click (docs/adr/0003, "button feedback").
  buzzer::patternButtonEmpty();
  Serial.print("focus: ");
  Serial.println(screenTitle(g_focus));
}

void handleButtons() {
  // Drain both buttons every iteration. Long presses are accepted by the
  // state machine but have no action yet, so they are logged and dropped.
  // Every accepted SHORT press is acknowledged with a short click, whether
  // or not it changed anything on screen (docs/adr/0003, "button feedback").
  const button::Event page = button::drain(button::BTN_PAGE);
  if (page == button::EVENT_SHORT_PRESS) {
    onButtonPage();
  } else if (page == button::EVENT_LONG_PRESS) {
    Serial.println("btn A long: no action");
  }

  const button::Event focus = button::drain(button::BTN_FOCUS);
  if (focus == button::EVENT_SHORT_PRESS) {
    onButtonFocus();
  } else if (focus == button::EVENT_LONG_PRESS) {
    Serial.println("btn B long: no action");
  }
}

// ---- network + buzzer edges -------------------------------------------------

// Turn the current state into events (docs/adr/0011). Called once per loop
// after health::update(), so an edge is seen within one iteration of it
// happening.
//
// When several edges fire in the same iteration they are played in the
// ADR-0011 order (new mail, then staleness, then link loss). The patterns
// are blocking, so "played in order" means the later ones start after the
// earlier ones finish -- which is the intended behaviour: a queue, not an
// overlap, because overlapping buzzes are indistinguishable from one long
// broken one.
void handleEdges() {
  // Two separate link facts, two separate edges. "Associated but no route
  // out" is its own condition (bench-measured 2026-10-17): the WiFi stack
  // never reports a drop, so netOnline stays true and only the link probe
  // reveals it. Announcing it as "wifi lost" would be a lie; announcing it
  // as "server stale" would blame the server for a local network fault.
  const bool link_usable = health::linkUsable();
  const bool net_online = health::netOnline();
  if (!net_online && g_prev_net_online) {
    // Rising edge: the association dropped.
    buzzer::patternWifiLost();
    Serial.println("edge: wifi lost");
  }
  if (net_online && !link_usable && g_prev_link_usable) {
    // Rising edge: still associated, but traffic no longer leaves.
    buzzer::patternWifiLost();
    Serial.println("edge: wifi up but no route out");
  }
  g_prev_net_online = net_online;
  g_prev_link_usable = link_usable;

  // Staleness is only meaningful while the link is usable; while it is not,
  // the link edges above already spoke, and re-announcing staleness every
  // time would be double notification.
  const bool stale = health::stale();
  if (stale && !g_prev_stale && link_usable) {
    buzzer::patternServerStale();
    Serial.println("edge: server data stale");
  }
  g_prev_stale = stale;

  // New mail. Consumed here so exactly one pattern plays per rising edge,
  // however many loop iterations pass before the next mailbox poll.
  if (health::unreadRose()) {
    buzzer::patternNewMail();
    Serial.println("edge: new mail");
  }

  // The data screens change when a poll lands -- that is the event, not
  // "the link is up". health::pollCount()/mailboxPollCount() are the counters
  // that move exactly then, so a rising count is the invalidate trigger.
  //
  // The previous code invalidated on net_online alone, i.e. once per loop()
  // iteration, which is once per few milliseconds: both data screens were
  // permanently dirty and got a full redraw + 8-page flush every round. That
  // quietly defeated the whole "nothing dirty -> zero bus traffic" design
  // documented at the top of this file.
  const uint32_t polls = health::pollCount();
  const uint32_t mail_polls = health::mailboxPollCount();
  if (polls != g_prev_poll_count || mail_polls != g_prev_mailbox_poll_count) {
    g_prev_poll_count = polls;
    g_prev_mailbox_poll_count = mail_polls;
    screen::invalidate(screen::SCREEN_SERVER);
    screen::invalidate(screen::SCREEN_MAILBOX);
  }
}

// ---- serial report --------------------------------------------------------

void printBootReport() {
  Serial.println();
  Serial.println("=== status-oled3 M1b boot report ===");
  if (pca9548a::isDirect()) {
    // One panel on the bus directly: reporting "pca9548a ABSENT" would be
    // noise, and reporting "screen ch0/ch1/ch2" would be a lie -- there is
    // one panel, not three.
    Serial.print("topology           : 1 panel direct on GPIO");
    Serial.print(static_cast<unsigned>(config::I2C_SDA_PIN));
    Serial.print("/GPIO");
    Serial.println(static_cast<unsigned>(config::I2C_SCL_PIN));
    Serial.print("panel              : ");
    Serial.println(screen::online(screen::SCREEN_LOCAL) ? "online" : "absent");
  } else {
    Serial.print("pca9548a 0x70      : ");
    Serial.println(pca9548a::ready() ? "online" : "ABSENT");
    for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
      const screen::ScreenId id = static_cast<screen::ScreenId>(i);
      Serial.print("screen ch");
      Serial.print(static_cast<unsigned>(config::SCREEN_CHANNEL[i]));
      Serial.print(" (");
      Serial.print(screenTitle(id));
      Serial.print(") : ");
      Serial.println(screen::online(id) ? "online" : "absent");
    }
  }
  Serial.print("focused screen     : ");
  Serial.println(screenTitle(g_focus));
  Serial.print("data source        : https://");
  Serial.print(config::DATA_HOST);
  Serial.print(config::DATA_PATH);
  Serial.print(" + ");
  Serial.println(config::MAILBOX_PATH);
  Serial.print("poll interval      : ");
  Serial.print(static_cast<unsigned>(config::POLL_INTERVAL_MS / 1000UL));
  Serial.print(" s, stale after ");
  Serial.print(static_cast<unsigned>(config::STALE_AFTER_MS / 1000UL));
  Serial.print(" s; mailbox every ");
  Serial.print(static_cast<unsigned>(config::MAILBOX_INTERVAL_MS / 1000UL));
  Serial.print(" s, stale after ");
  Serial.print(static_cast<unsigned>(config::MAILBOX_STALE_AFTER_MS / 1000UL));
  Serial.println(" s");
  Serial.print("pages              : local=");
  Serial.print(static_cast<unsigned>(g_page_count[screen::SCREEN_LOCAL]));
  Serial.print(" server=");
  Serial.print(static_cast<unsigned>(g_page_count[screen::SCREEN_SERVER]));
  Serial.print(" mailbox=");
  Serial.println(static_cast<unsigned>(g_page_count[screen::SCREEN_MAILBOX]));
  Serial.println("==================================");
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give the USB CDC port a moment to enumerate before the first print, so
  // the boot report is not lost. Not fatal if it never attaches.
  delay(300);

  buzzer::begin();
  button::begin();
  health::begin();

  // The panel comes up before the screens: every screen probe rides the bus.
  // screen::begin() calls pca9548a::begin() internally, which initialises the
  // bus even in the single-panel topology; the report below then states what
  // actually happened.
  screen::begin();

  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    screen::setPageCount(id, g_page_count[i]);
  }

  printBootReport();

  // First paint: with three panels every screen is dirty right after begin(),
  // so this draws all three immediately instead of waiting for the first 1 s
  // tick. With one panel only the focused screen is showable, so this paints
  // exactly the one the user sees at power-on (docs/adr/0001 default).
  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    if (screen::showable(id)) {
      renderScreen(id);
    }
  }
  screen::setFocused(g_focus);
  // Seed every cadence from the same clock reading taken AFTER screen::begin()
  // and printBootReport(), both of which block. Seeding g_last_rescan_ms too:
  // it used to stay 0, so the first loop() iteration saw a huge elapsed time
  // and ran a rescan immediately -- a bus burst before the first paint had
  // even been pushed.
  const uint32_t now = millis();
  g_last_local_ms = now;
  g_last_netlog_ms = now;
  g_last_rescan_ms = now;
}

void loop() {
  const uint32_t now = millis();

  // Buttons first: a press must never be delayed by a screen refresh.
  button::tick();
  handleButtons();

  // Network: the 15 s snapshot poll, the 5 min mailbox poll and the 5 s link
  // check. This is where a blocking TLS request happens, so it runs after
  // the buttons and before the screens; the request is bounded by
  // HTTP_TIMEOUT_MS.
  health::update();
  handleEdges();

  // Periodic re-probe of absent screens, so unplug/replug recovers without
  // a reset. Cheap: it only touches channels that are currently offline, and
  // a probe that ACKs costs one I2C transaction (no 6 s self-check any more --
  // fontDirProbe() is no longer called from begin()).
  if (elapsedSince(now, g_last_rescan_ms) >= kRescanIntervalMs) {
    g_last_rescan_ms = now;
    screen::rescan();
  }

  // The ESP32 status screen carries a 1 s-changing number (uptime), so it
  // is redrawn on a fixed cadence. The server/mailbox screens are redrawn
  // whenever a poll lands or focus moves (see handleEdges).
  if (elapsedSince(now, g_last_local_ms) >= config::REFRESH_LOCAL_MS) {
    g_last_local_ms = now;
    screen::invalidate(screen::SCREEN_LOCAL);
  }

  // Draw and push whatever is dirty. With nothing dirty this is three flag
  // checks and zero bus traffic. renderScreen() always draws then flushes, so
  // a screen that was invalidated by a button press is painted in the same
  // iteration the press arrived.
  //
  // showable() (not online()) is the gate: with one panel the two non-focused
  // screens are dirty every second -- their content changes on their own
  // cadences -- and painting them would overwrite the panel the user is
  // actually looking at.
  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    if (screen::showable(id) && screen::isDirty(id)) {
      renderScreen(id);
    }
  }

  // One network line per minute: enough to see the trend without flooding
  // the log during a long uptime.
  if (elapsedSince(now, g_last_netlog_ms) >= kNetLogIntervalMs) {
    g_last_netlog_ms = now;
    health::printStatus();
  }
}
