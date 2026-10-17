/**
 * main.cpp -- status-oled3 firmware, M0 (hardware bringup) + M1a (data path)
 *
 * M0 scope (see README.md milestone table): the PCA9548A + three SSD1306
 * chain, the two buttons, and the buzzer. No WiFi, no aggregator polling --
 * that is M1. Everything on screen here is either hardware state or a
 * placeholder, so that a "broken" screen can be told apart from a
 * "no data yet" screen by eye.
 *
 * M1a scope (docs/adr/0009): the device-side network path only -- WiFi,
 * HTTPS GET of the main site's public /api/health, JSON field extraction,
 * the 15 s poll cadence, the 60 s staleness verdict, and the two buzzer
 * edges that data can produce (WiFi lost, data stale). The server side is
 * untouched at this stage, so the ESP32 screen shows real RSSI/IP/heap and
 * the server screen shows the real process uptime and commit.
 *
 * Structure: four independent cadences sharing one loop().
 *   - buttons: drained every iteration (a missed press is a lost press)
 *   - network: 15 s poll, 5 s link check (see health.cpp)
 *   - ESP32 status screen: 1 s (uptime changes every second)
 *   - the other two screens: only when invalidated
 * The screen layer owns the dirty flag, so an iteration with nothing to do
 * costs no I2C traffic at all.
 */
#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

#include "button.h"
#include "buzzer.h"
#include "config.h"
#include "health.h"
#include "pca9548a.h"
#include "screen.h"

namespace {

// ---- focus / page state (the M0 interaction model) ------------------------
// Which screen currently answers to the buttons. Power-on default is the
// ESP32 status screen (docs/adr/0001).
screen::ScreenId g_focus = screen::SCREEN_LOCAL;

// Page count of each screen. 1 everywhere at M0: the server screen's two
// pages need real data to be worth showing, and the mailbox screen has a
// single page by design. ADR-0001 keeps the roles fixed, so these counts are
// set once here instead of being derived per draw.
uint8_t g_page_count[screen::SCREEN_COUNT] = {1, 1, 1};

// ---- refresh timing -------------------------------------------------------
uint32_t g_last_local_ms = 0;
uint32_t g_last_rescan_ms = 0;
uint32_t g_last_netlog_ms = 0;

// ---- buzzer edges (docs/adr/0011) -----------------------------------------
// The three "previous" values that turn a state into an event. The initial
// values are chosen so that booting never fires a pattern by itself:
//   prevNetOnline = true  -> the first "down" is a real transition
//   prevStale     = false -> the first "stale" is a real transition
//   prevUnread    = 0     -> set to the real value after the first fetch,
//                            so "0 -> 7" is not mistaken for new mail
// (mailbox stays at M1 pending until M1b, so prevUnread is unused for now).
bool g_prev_net_online = true;
bool g_prev_link_usable = false;
bool g_prev_stale = false;

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
  // rather than being buried in the serial log.
  char online_flags[5];
  online_flags[0] = screen::online(screen::SCREEN_LOCAL) ? '1' : '0';
  online_flags[1] = screen::online(screen::SCREEN_SERVER) ? '1' : '0';
  online_flags[2] = screen::online(screen::SCREEN_MAILBOX) ? '1' : '0';
  online_flags[3] = '\0';
  screen::printKV(screen::SCREEN_LOCAL, 4, "scr", online_flags);

  // IP: the last octet only -- a full dotted quad does not fit next to a
  // label at 6 px per character, and the last octet is what tells two
  // devices apart on the same network.
  if (health::netOnline()) {
    const IPAddress ip = WiFi.localIP();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    screen::printKV(screen::SCREEN_LOCAL, 5, "ip", buf);
  } else {
    screen::printKV(screen::SCREEN_LOCAL, 5, "ip", "-");
  }

  screen::printKV(screen::SCREEN_LOCAL, 6, "btn", "A=page B=focus");
}

void drawServer() {
  if (!screen::online(screen::SCREEN_SERVER)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_SERVER, "SERVER",
                       g_focus == screen::SCREEN_SERVER);

  // M1a: the server screen carries what /api/health can actually answer --
  // the Go process's own uptime and its build identity. It deliberately
  // does NOT show CPU/memory/disk: those come with the M1b snapshot
  // (docs/adr/0008 consequence 1 says this is a real, if thin, data view,
  // not a placeholder).
  const char* up = health::serverUptime();
  screen::printKV(screen::SCREEN_SERVER, 1, "up",
                  (up != nullptr && up[0] != '\0') ? up : "-");
  screen::printKV(screen::SCREEN_SERVER, 2, "commit",
                  health::serverCommit()[0] != '\0' ? health::serverCommit()
                                                    : "-");

  // Freshness instead of a page counter: at one page there is nothing to
  // count, and "how old is this data" is the number that matters now.
  if (!health::netOnline()) {
    screen::printKV(screen::SCREEN_SERVER, 3, "data", "wifi down");
  } else if (!health::linkUsable()) {
    screen::printKV(screen::SCREEN_SERVER, 3, "data", "no route out");
  } else if (health::stale()) {
    char buf[32];
    const uint32_t age = health::secondsSinceData();
    if (age == 0) {
      screen::printKV(screen::SCREEN_SERVER, 3, "data", "no data yet");
    } else {
      snprintf(buf, sizeof(buf), "stale %lus", static_cast<unsigned long>(age));
      screen::printKV(screen::SCREEN_SERVER, 3, "data", buf);
    }
  } else {
    screen::printKV(screen::SCREEN_SERVER, 3, "data", "fresh");
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(health::pollCount()));
  screen::printKV(screen::SCREEN_SERVER, 4, "polls", buf);
  screen::printKV(screen::SCREEN_SERVER, 6, "focus", "B to switch");
}

void drawMailbox() {
  if (!screen::online(screen::SCREEN_MAILBOX)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_MAILBOX, "MAILBOX",
                       g_focus == screen::SCREEN_MAILBOX);
  // M1b fills this in (the mailbox cron writes mailbox.json, docs/adr/0008).
  // Until then the honest placeholder stays: it is distinguishable from a
  // broken screen and from a real "0 unread".
  screen::printKV(screen::SCREEN_MAILBOX, 1, "ch", "0x3C/9548A");
  screen::printKV(screen::SCREEN_MAILBOX, 2, "unread", "M1 pending");
  screen::printKV(screen::SCREEN_MAILBOX, 3, "focus", "B to switch");
}

// Redraw one screen's content, then push it.
void renderScreen(screen::ScreenId id) {
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
  // state machine but have no M0 action, so they are logged and dropped.
  // Every accepted SHORT press is acknowledged with a short click, whether
  // or not it changed anything on screen (docs/adr/0003, "button feedback").
  const button::Event page = button::drain(button::BTN_PAGE);
  if (page == button::EVENT_SHORT_PRESS) {
    onButtonPage();
  } else if (page == button::EVENT_LONG_PRESS) {
    Serial.println("btn A long: no action at M0");
  }

  const button::Event focus = button::drain(button::BTN_FOCUS);
  if (focus == button::EVENT_SHORT_PRESS) {
    onButtonFocus();
  } else if (focus == button::EVENT_LONG_PRESS) {
    Serial.println("btn B long: no action at M0");
  }
}

// ---- network + buzzer edges -------------------------------------------------

// Turn the current state into events (docs/adr/0011). Called once per loop
// after health::update(), so an edge is seen within one iteration of it
// happening.
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

  // The data screens only change when a poll lands or focus moves, so they
  // are invalidated here rather than on a timer.
  if (net_online) {
    screen::invalidate(screen::SCREEN_SERVER);
    screen::invalidate(screen::SCREEN_MAILBOX);
  }
}

// ---- serial report --------------------------------------------------------

void printBootReport() {
  Serial.println();
  Serial.println("=== status-oled3 M1a boot report ===");
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
  Serial.println(config::DATA_PATH);
  Serial.print("poll interval      : ");
  Serial.print(static_cast<unsigned>(config::POLL_INTERVAL_MS / 1000UL));
  Serial.print(" s, stale after ");
  Serial.print(static_cast<unsigned>(config::STALE_AFTER_MS / 1000UL));
  Serial.println(" s");
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
  g_last_local_ms = millis();
  g_last_netlog_ms = millis();
}

void loop() {
  const uint32_t now = millis();

  // Buttons first: a press must never be delayed by a screen refresh.
  button::tick();
  handleButtons();

  // Network: the 15 s poll and the 5 s link check. This is where a blocking
  // TLS request happens, so it runs after the buttons and before the
  // screens; the request is bounded by HTTP_TIMEOUT_MS.
  health::update();
  handleEdges();

  // Periodic re-probe of absent screens, so unplug/replug recovers without
  // a reset. Cheap: it only touches channels that are currently offline.
  if (now - g_last_rescan_ms >= kRescanIntervalMs) {
    g_last_rescan_ms = now;
    screen::rescan();
  }

  // The ESP32 status screen carries a 1 s-changing number (uptime), so it
  // is redrawn on a fixed cadence. The server/mailbox screens are redrawn
  // whenever a poll lands or focus moves (see handleEdges).
  if (now - g_last_local_ms >= config::REFRESH_LOCAL_MS) {
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
  if (now - g_last_netlog_ms >= kNetLogIntervalMs) {
    g_last_netlog_ms = now;
    health::printStatus();
  }
}
