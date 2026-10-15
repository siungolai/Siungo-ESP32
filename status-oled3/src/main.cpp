/**
 * main.cpp -- status-oled3 firmware, M0 (hardware bringup)
 *
 * M0 scope (see README.md milestone table): the PCA9548A + three SSD1306
 * chain, the two buttons, and the buzzer. No WiFi, no aggregator polling --
 * that is M1. Everything on screen here is either hardware state or a
 * placeholder, so that a "broken" screen can be told apart from a
 * "no data yet" screen by eye.
 *
 * Structure: three independent cadences sharing one loop().
 *   - buttons: drained every iteration (a missed press is a lost press)
 *   - ESP32 status screen: 1 s (uptime changes every second)
 *   - the other two screens: only when invalidated
 * The screen layer owns the dirty flag, so an iteration with nothing to do
 * costs no I2C traffic at all.
 */
#include <Arduino.h>

#include <cstdio>

#include "button.h"
#include "buzzer.h"
#include "config.h"
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

// A screen unplugged and replugged should come back on its own.
constexpr uint32_t kRescanIntervalMs = config::RESCAN_INTERVAL_MS;

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

  // WiFi comes in at M1; until then this line is the honest placeholder.
  screen::printKV(screen::SCREEN_LOCAL, 2, "wifi", "M1 pending");

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

  screen::printKV(screen::SCREEN_LOCAL, 5, "btn", "A=page B=focus");

  // Page indicator. Present on every screen so the interaction model is
  // discoverable without the README.
  snprintf(buf, sizeof(buf), "%u/%u",
           static_cast<unsigned>(screen::pageIndex(screen::SCREEN_LOCAL)) + 1,
           static_cast<unsigned>(screen::pageCount(screen::SCREEN_LOCAL)));
  screen::printKV(screen::SCREEN_LOCAL, 6, "pg", buf);
}

void drawServer() {
  if (!screen::online(screen::SCREEN_SERVER)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_SERVER, "SERVER",
                       g_focus == screen::SCREEN_SERVER);
  // M0: no data path yet. Show the hardware truth (which channel is live)
  // plus a plain "waiting for M1" so it cannot be mistaken for a broken
  // screen.
  screen::printKV(screen::SCREEN_SERVER, 1, "ch", "0x3C/9548A");
  screen::printKV(screen::SCREEN_SERVER, 2, "data", "M1 pending");
  screen::printKV(screen::SCREEN_SERVER, 3, "focus", "B to switch");

  char buf[32];
  snprintf(buf, sizeof(buf), "%u/%u",
           static_cast<unsigned>(screen::pageIndex(screen::SCREEN_SERVER)) + 1,
           static_cast<unsigned>(screen::pageCount(screen::SCREEN_SERVER)));
  screen::printKV(screen::SCREEN_SERVER, 6, "pg", buf);
}

void drawMailbox() {
  if (!screen::online(screen::SCREEN_MAILBOX)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_MAILBOX, "MAILBOX",
                       g_focus == screen::SCREEN_MAILBOX);
  screen::printKV(screen::SCREEN_MAILBOX, 1, "ch", "0x3C/9548A");
  screen::printKV(screen::SCREEN_MAILBOX, 2, "unread", "M1 pending");
  screen::printKV(screen::SCREEN_MAILBOX, 3, "focus", "B to switch");

  char buf[32];
  snprintf(buf, sizeof(buf), "%u/%u",
           static_cast<unsigned>(screen::pageIndex(screen::SCREEN_MAILBOX)) + 1,
           static_cast<unsigned>(screen::pageCount(screen::SCREEN_MAILBOX)));
  screen::printKV(screen::SCREEN_MAILBOX, 6, "pg", buf);
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
  if (screen::pageCount(g_focus) <= 1) {
    buzzer::patternButtonEmpty();
    Serial.println("page: focused screen has 1 page, nothing to flip");
    return;
  }
  screen::nextPage(g_focus);
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

  // Focus is drawn on every title bar, so all three screens must be redrawn,
  // not just the one that lost or gained focus.
  screen::invalidate(screen::SCREEN_LOCAL);
  screen::invalidate(screen::SCREEN_SERVER);
  screen::invalidate(screen::SCREEN_MAILBOX);
  Serial.print("focus: ");
  Serial.println(screenTitle(g_focus));
}

void handleButtons() {
  // Drain both buttons every iteration. Long presses are accepted by the
  // state machine but have no M0 action, so they are logged and dropped.
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

// ---- serial report --------------------------------------------------------

void printBootReport() {
  Serial.println();
  Serial.println("=== status-oled3 M0 boot report ===");
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
  Serial.print("focused screen     : ");
  Serial.println(screenTitle(g_focus));
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

  // The mux comes up before the screens: every screen probe rides it.
  // screen::begin() calls pca9548a::begin() internally; the explicit call
  // below is here only so the serial report can state the mux's own status
  // even when all three screens are absent.
  screen::begin();

  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    screen::setPageCount(id, g_page_count[i]);
  }

  printBootReport();

  // First paint: every screen is dirty right after begin(), so this draws
  // all three immediately instead of waiting for the first 1 s tick.
  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    renderScreen(static_cast<screen::ScreenId>(i));
  }
  g_last_local_ms = millis();
}

void loop() {
  const uint32_t now = millis();

  // Buttons first: a press must never be delayed by a screen refresh.
  button::tick();
  handleButtons();

  // Periodic re-probe of absent screens, so unplug/replug recovers without
  // a reset. Cheap: it only touches channels that are currently offline.
  if (now - g_last_rescan_ms >= kRescanIntervalMs) {
    g_last_rescan_ms = now;
    screen::rescan();
  }

  // The ESP32 status screen carries a 1 s-changing number (uptime), so it
  // is redrawn on a fixed cadence. The other two screens are redrawn only
  // when something invalidates them (focus change, rescan recovery), which
  // at M0 is exactly as often as their content changes: never.
  if (now - g_last_local_ms >= config::REFRESH_LOCAL_MS) {
    g_last_local_ms = now;
    screen::invalidate(screen::SCREEN_LOCAL);
  }

  // Draw and push whatever is dirty. With nothing dirty this is three flag
  // checks and zero bus traffic. renderScreen() always draws then flushes, so
  // a screen that was invalidated by a button press is painted in the same
  // iteration the press arrived.
  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    if (screen::online(id) && screen::isDirty(id)) {
      renderScreen(id);
    }
  }
}
