/**
 * main.cpp -- pet-oled3 firmware, M0 (hardware bringup)
 *
 * Scope (docs/策划案.md section 6, M0): prove the hardware path works before
 * any game logic exists. Concretely this firmware:
 *
 *   1. brings up the PCA9548A and probes all three channels, and prints the
 *      RAW endTransmission() return code of every channel probe -- the M0
 *      checklist asks whether any code 5 (timeout) ever appears, and a
 *      boolean "online" cannot answer that;
 *   2. paints three DIFFERENT contents on the three panels, so "the mux
 *      really switched" is visible by eye and not merely asserted;
 *   3. reads the buzzer pin's pad drive capability back from the hardware,
 *      so ADR-0010's requirement is a measured fact and not a promise;
 *   4. plays every buzzer pattern once at boot, so the operator can tick
 *      them off by ear (the active buzzer has a fixed pitch, so the patterns
 *      are told apart by COUNT and DURATION);
 *   5. turns the two buttons into logged events, so short press, long press
 *      and debouncing can all be checked from the serial log and from the
 *      log screen.
 *
 * What is deliberately NOT here: the three numbers do not tick, the growth
 * stage does not advance, the actions do not change any value. Those are M3
 * (game core) and M1 (input primitives). The stats screen shows the M0
 * initial values from docs/策划案.md section 4.6 and says so on screen, so
 * a static panel is never mistaken for a live one.
 *
 * The pet face is the first non-text content in this project, and it goes
 * through screen::drawBitmap() -- the one path that puts pixels on a panel
 * without handing out the raw framebuffer.
 */
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

#include <cstdio>

#include "button.h"
#include "buzzer.h"
#include "config.h"
#include "pca9548a.h"
#include "screen.h"

namespace {

// ---- pet face (16x16) ------------------------------------------------------
// One byte = one column, bit0 = the top row of the 16-row span. With
// rowsHeight = 16 the low byte of each column is the UPPER half and the
// high byte the LOWER half (the Chinese glyph table's layout).
//
// The shape is a filled head with two ears, two eye holes and a mouth hole:
// on a monochrome panel the features are HOLES in a lit face, so the eyes
// and the mouth are pixels that are NOT set.
//
// Column layout (16 columns, rows 0-15 top to bottom):
//   cols 2,3 and 12,13  the two ears (rows 0-5, merged into the head)
//   cols 4-11          the head (rows 2-13)
//   cols 5,6           left eye   (rows 5,6 are holes)
//   cols 9,10          right eye  (rows 5,6 are holes)
//   cols 7,8           the mouth  (rows 10,11 are holes)
const uint8_t kFace[16][2] = {
    {0x00, 0x00},  // col 0
    {0x00, 0x00},  // col 1
    {0x3F, 0x00},  // col 2   left ear, rows 0-5
    {0xFF, 0x3F},  // col 3   ear into head, rows 0-13
    {0xFC, 0x3F},  // col 4   head, rows 2-13
    {0x9C, 0x3F},  // col 5   head, eye hole
    {0x9C, 0x3F},  // col 6   head, eye hole
    {0xFC, 0x33},  // col 7   head, mouth hole
    {0xFC, 0x33},  // col 8   head, mouth hole
    {0x9C, 0x3F},  // col 9   head, eye hole
    {0x9C, 0x3F},  // col 10  head, eye hole
    {0xFC, 0x3F},  // col 11  head, rows 2-13
    {0xFF, 0x3F},  // col 12  head into ear, rows 0-13
    {0x3F, 0x00},  // col 13  right ear, rows 0-5
    {0x00, 0x00},  // col 14
    {0x00, 0x00},  // col 15
};

// Where the face goes. The panel is 128x64; the face is 16x16, so x = 104
// leaves it inside the right edge and clear of the 12-character text column
// on the left (12 chars x 6 px = 72 px).
constexpr uint8_t kFaceX = 104;
constexpr uint8_t kFaceY = 16;

// The three actions, in cursor order (docs/策划案.md section 4.4).
const char* const kActionNames[3] = {"FEED", "PLAY", "REST"};

// Which action the cursor is on. Button A moves it; button B "executes" it.
uint8_t g_cursor = 0;
// The last action button B accepted, or "-" before the first one.
const char* g_last_action = "-";

// ---- log ring buffer -------------------------------------------------------
// The log screen shows the last kLogRows events. It is a ring buffer rather
// than a growing array because a panel is 64 px tall and cannot show more
// than seven rows anyway.
constexpr uint8_t kLogRows = 7;
constexpr uint8_t kLogTextLen = 22;
char g_log[kLogRows][kLogTextLen];
uint8_t g_log_head = 0;   // next slot to write
uint8_t g_log_count = 0;  // how many slots hold a real line

// ---- tick state ------------------------------------------------------------
uint32_t g_tick_count = 0;
uint32_t g_last_tick_ms = 0;
uint32_t g_last_rescan_ms = 0;
uint32_t g_last_heartbeat_ms = 0;

// ---- formatting helpers ----------------------------------------------------

// Seconds -> "12s" / "3m20s" / "1h02m". The log and the stats screen both
// want a short uptime, and "3661s" is neither short nor readable.
void formatUptime(char* out, size_t n, uint32_t seconds) {
  const uint32_t hours = seconds / 3600UL;
  const uint32_t mins = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;
  if (hours > 0) {
    snprintf(out, n, "%luh%02lum", static_cast<unsigned long>(hours),
             static_cast<unsigned long>(mins));
  } else if (mins > 0) {
    snprintf(out, n, "%lum%02lus", static_cast<unsigned long>(mins),
             static_cast<unsigned long>(secs));
  } else {
    snprintf(out, n, "%lus", static_cast<unsigned long>(secs));
  }
}

// A raw endTransmission() code as a short name, so the boot report reads
// like a diagnosis rather than like a list of integers.
const char* i2cCodeName(uint8_t code) {
  switch (code) {
    case 0: return "ACK";
    case 1: return "data_nack";
    case 2: return "addr_nack";
    case 3: return "nack";
    case 4: return "other_error";
    case 5: return "TIMEOUT";
    default: return "?";
  }
}

// ---- logging ---------------------------------------------------------------

// Draw the log screen from the ring buffer. Oldest line on the top row.
//
// showable() rather than online(): the contract in screen.h is that a caller
// decides what to paint with showable(). With the three-panel rig of ADR-0001
// the two are the same, because all three panels are wired.
void drawLogScreen() {
  if (!screen::showable(screen::SCREEN_LOG)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_LOG, "LOG", false);
  for (uint8_t i = 0; i < g_log_count; ++i) {
    // The oldest of the g_log_count lines sits (head - count) slots back.
    const uint8_t idx =
        static_cast<uint8_t>((g_log_head + kLogRows - g_log_count + i) % kLogRows);
    screen::printLine(screen::SCREEN_LOG, static_cast<uint8_t>(i + 1), g_log[idx]);
  }
  screen::flush(screen::SCREEN_LOG);
}

// Append one line to the log ring and repaint the log screen.
//
// The repaint happens right here rather than on the next tick: an event that
// shows up a second later is indistinguishable from a lost event when the
// panel is 64 px tall and the operator is watching the buzzer.
void logMsg(const char* text) {
  snprintf(g_log[g_log_head], kLogTextLen, "%s", text);
  g_log_head = (g_log_head + 1) % kLogRows;
  if (g_log_count < kLogRows) {
    ++g_log_count;
  }
  // The same line goes to Serial. The log screen is 64 px tall and holds
  // seven lines, so it is a display, not a record: without the serial copy
  // a button event is gone the moment it scrolls off, and there is no way
  // to prove a gesture fired after the fact. The serial line is the
  // evidence; the panel line is the convenience.
  Serial.print("[evt] ");
  Serial.println(text);
  drawLogScreen();
}

// ---- the three screens -----------------------------------------------------

// Role screen: the pet. A face bitmap plus the three action names with the
// cursor marked, plus the two lines that say what the buttons do.
//
// The cursor marker is a '>' in front of the selected action and a space in
// front of the others. It is drawn in the same text column for all three
// rows, so the marker never moves.
void drawRoleScreen() {
  if (!screen::showable(screen::SCREEN_ROLE)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_ROLE, "PET", false);

  char buf[32];
  snprintf(buf, sizeof(buf), "%s%s", (g_cursor == 0) ? ">" : " ", kActionNames[0]);
  screen::printLine(screen::SCREEN_ROLE, 1, buf);
  snprintf(buf, sizeof(buf), "%s%s", (g_cursor == 1) ? ">" : " ", kActionNames[1]);
  screen::printLine(screen::SCREEN_ROLE, 2, buf);
  snprintf(buf, sizeof(buf), "%s%s", (g_cursor == 2) ? ">" : " ", kActionNames[2]);
  screen::printLine(screen::SCREEN_ROLE, 3, buf);

  screen::printLine(screen::SCREEN_ROLE, 4, "A: move");
  screen::printLine(screen::SCREEN_ROLE, 5, "B: act");
  snprintf(buf, sizeof(buf), "last: %s", g_last_action);
  screen::printLine(screen::SCREEN_ROLE, 6, buf);

  // The face. Drawn AFTER the title bar: drawTitleBar() clears the
  // framebuffer, so a bitmap drawn before it would be wiped.
  screen::drawBitmap(screen::SCREEN_ROLE, kFaceX, kFaceY, &kFace[0][0], 16, 16);

  screen::flush(screen::SCREEN_ROLE);
}

// Stats screen: the three numbers at their M0 initial values, plus the tick
// counter and the uptime, so the 1 Hz tick is visible.
//
// The values are STATIC at M0. The line "M0 static" says so on the panel:
// a number that never changes must never look like a live one.
void drawStatsScreen() {
  if (!screen::showable(screen::SCREEN_STATS)) {
    return;
  }
  screen::drawTitleBar(screen::SCREEN_STATS, "STATS", false);

  // Initial values from docs/策划案.md section 4.6 (M3 makes them live).
  //
  // These keys are PANEL-ONLY short labels: the 5x7 dot-matrix font has no
  // Chinese glyphs, and M0 only shows the static initial values. They are NOT
  // the official code identifiers -- CONTEXT.md says those are still undecided
  // and must not be invented before M3.
  screen::printKV(screen::SCREEN_STATS, 1, "food", "70");
  screen::printKV(screen::SCREEN_STATS, 2, "mood", "70");
  screen::printKV(screen::SCREEN_STATS, 3, "energy", "80");
  screen::printKV(screen::SCREEN_STATS, 4, "aff", "0");

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(g_tick_count));
  screen::printKV(screen::SCREEN_STATS, 5, "tick", buf);
  formatUptime(buf, sizeof(buf), millis() / 1000UL);
  screen::printKV(screen::SCREEN_STATS, 6, "up", buf);
  screen::printLine(screen::SCREEN_STATS, 7, "M0 static");

  screen::flush(screen::SCREEN_STATS);
}

// ---- buttons ---------------------------------------------------------------

void onButtonA() {
  g_cursor = (g_cursor + 1) % 3;
  char buf[32];
  snprintf(buf, sizeof(buf), "A short -> %s", kActionNames[g_cursor]);
  logMsg(buf);
  buzzer::patternKeyClick();
  drawRoleScreen();
}

void onButtonB() {
  g_last_action = kActionNames[g_cursor];
  char buf[32];
  snprintf(buf, sizeof(buf), "B short -> %s", g_last_action);
  logMsg(buf);
  // Two sounds, in this order: the click acknowledges the press, the
  // action beep says the action was taken. At M0 the action changes no
  // number yet, so the sound announces the intent only.
  buzzer::patternKeyClick();
  buzzer::patternActionDone();
  drawRoleScreen();
}

// ---- boot report -----------------------------------------------------------

// Probe one mux channel and return the RAW endTransmission() code of the
// SSD1306 behind it.
//
// This goes straight to Wire instead of using screen::online() on purpose:
// screen::online() answers with a boolean, and the M0 checklist asks
// whether any transaction returned 5 (timeout). A boolean cannot tell
// "timeout" apart from "address NACK", and those two have completely
// different causes.
//
// The mux is selected for exactly the duration of the probe and
// disconnected afterwards, which is the one-channel-at-a-time rule the
// whole design rests on.
uint8_t probeChannel(uint8_t channel) {
  if (!pca9548a::select(channel)) {
    return 0xFF;  // could not even select the channel: not a bus code
  }
  Wire.beginTransmission(config::SSD1306_ADDR);
  const uint8_t code = Wire.endTransmission();
  pca9548a::deselectAll();
  return code;
}

void printBootReport() {
  Serial.println();
  Serial.println("=== pet-oled3 M0 boot report ===");

  Serial.print("i2c                : SDA=GPIO");
  Serial.print(static_cast<unsigned>(config::I2C_SDA_PIN));
  Serial.print(" SCL=GPIO");
  Serial.print(static_cast<unsigned>(config::I2C_SCL_PIN));
  Serial.print(" @");
  Serial.print(static_cast<unsigned>(config::I2C_HZ));
  Serial.print("Hz, timeout ");
  Serial.print(static_cast<unsigned>(config::I2C_TIMEOUT_MS));
  Serial.println("ms");

  Serial.print("pca9548a 0x70      : ");
  Serial.println(pca9548a::ready() ? "online" : "ABSENT");

  for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
    const screen::ScreenId id = static_cast<screen::ScreenId>(i);
    const uint8_t ch = config::SCREEN_CHANNEL[i];
    const uint8_t code = probeChannel(ch);
    Serial.print("ch");
    Serial.print(static_cast<unsigned>(ch));
    Serial.print(" code=");
    Serial.print(code);
    Serial.print(" (");
    Serial.print(i2cCodeName(code));
    Serial.print(") screen ");
    Serial.print(screen::online(id) ? "online" : "ABSENT");
    Serial.println();
  }

  // The mux's own last error, which covers the channel SELECT writes too.
  Serial.print("mux last error     : ");
  Serial.print(pca9548a::lastError());
  Serial.print(" (");
  Serial.print(i2cCodeName(pca9548a::lastError()));
  Serial.println(")");

  // Drive capability of the buzzer pin, READ BACK from the hardware.
  // ADR-0010 says the pad must be raised to GPIO_DRIVE_CAP_3 (40 mA source
  // current) because the buzzer draws 20-30 mA and the default pad is
  // 20 mA. Printing the value that the register actually holds is the
  // difference between "the code says so" and "the hardware says so".
  gpio_drive_cap_t cap = GPIO_DRIVE_CAP_0;
  const esp_err_t err = gpio_get_drive_capability(
      static_cast<gpio_num_t>(config::PIN_BUZZER), &cap);
  Serial.print("buzzer pin         : GPIO");
  Serial.print(static_cast<unsigned>(config::PIN_BUZZER));
  Serial.print(" drive cap=");
  Serial.print(static_cast<int>(cap));
  Serial.print(" (want 3)");
  Serial.print(" readback ");
  Serial.print(err == ESP_OK ? "ok" : "FAILED");
  Serial.println();

  Serial.println("pattern check      : click / action / alert / levelup / unhappy");
  Serial.println("======================================");
  Serial.println();
}

// Play every buzzer pattern once, in the order of the rhythm table
// (docs/策划案.md section 4.4), so the operator can tick them off by ear.
//
// The active buzzer has a fixed pitch, so the patterns are told apart by
// COUNT and DURATION only:
//   click   1 x 20 ms   (key acknowledged)
//   action  1 x 80 ms   (an action took effect)
//   alert   3 x 80 ms   (a number crossed the threshold)
//   levelup 1 x 400 ms  (growth stage advanced)
//   unhappy 2 x 80 ms   (the pet is not happy with that)
void playPatternCheck() {
  buzzer::patternKeyClick();
  delay(300);
  buzzer::patternActionDone();
  delay(300);
  buzzer::patternAlert();
  delay(300);
  buzzer::patternLevelUp();
  delay(300);
  buzzer::patternUnhappy();
  delay(300);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give the USB CDC port a moment to enumerate before the first print, so
  // the boot report is not lost.
  delay(300);

  buzzer::begin();
  button::begin();

  // The mux comes up first: every screen probe rides it.
  screen::begin();

  printBootReport();

  // First paint of all three panels. Each one gets DIFFERENT content, which
  // is the only way to see by eye that the mux really switched channels.
  drawRoleScreen();
  drawStatsScreen();
  logMsg("boot: M0 firmware up");

  // The pattern check comes after the first paint, so the panels are already
  // showing their content when the buzzer starts -- otherwise the operator
  // hears five beeps and then has to wait for the picture.
  playPatternCheck();
  logMsg("patterns played");

  // Seed the cadences from one clock reading taken AFTER the blocking calls
  // above, so the first loop() iteration does not see a huge elapsed time
  // and fire a rescan and a tick immediately.
  const uint32_t now = millis();
  g_last_tick_ms = now;
  g_last_rescan_ms = now;
  g_last_heartbeat_ms = now;
}

void loop() {
  const uint32_t now = millis();

  // Buttons first: a press must never be delayed by a screen refresh.
  button::tick();

  const button::Event a = button::drain(button::BTN_PAGE);
  if (a == button::EVENT_SHORT_PRESS) {
    onButtonA();
  } else if (a == button::EVENT_LONG_PRESS) {
    // The long press is the menu gesture (docs/adr/0009). The menu itself
    // is M1, so at M0 the event is only proven to fire.
    logMsg("A long -> menu (M1)");
    buzzer::patternKeyClick();
  }

  const button::Event b = button::drain(button::BTN_FOCUS);
  if (b == button::EVENT_SHORT_PRESS) {
    onButtonB();
  } else if (b == button::EVENT_LONG_PRESS) {
    logMsg("B long -> back (M1)");
    buzzer::patternKeyClick();
  }

  // The 1 Hz tick. At M0 it only moves the counters; the numbers themselves
  // start ticking at M3.
  if (now - g_last_tick_ms >= config::TICK_INTERVAL_MS) {
    g_last_tick_ms = now;
    ++g_tick_count;
    drawStatsScreen();
  }

  // A heartbeat, once a minute, so the serial log proves the firmware is
  // still alive and still has all three panels. It replaces the old
  // unconditional 5-second rescan line, which repeated the same string
  // forever and buried the event log it was supposed to support.
  if (now - g_last_heartbeat_ms >= 60000UL) {
    g_last_heartbeat_ms = now;
    Serial.print("[hb] tick=");
    Serial.print(static_cast<unsigned long>(g_tick_count));
    Serial.print(" up=");
    char buf[32];
    formatUptime(buf, sizeof(buf), now / 1000UL);
    Serial.print(buf);
    Serial.print(" screens=");
    for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
      Serial.print(screen::online(static_cast<screen::ScreenId>(i)) ? '1' : '0');
    }
    Serial.print(" mux_err=");
    Serial.println(pca9548a::lastError());
  }

  // Periodic re-probe of absent panels, so unplug/replug recovers without a
  // reset. It only touches channels that are currently offline.
  //
  // The serial line is printed only when the set of online panels CHANGED.
  // An unconditional line every 5 s would drown the event log -- the very
  // thing the serial channel exists for -- and a line that always says the
  // same thing carries no information anyway.
  if (now - g_last_rescan_ms >= config::RESCAN_INTERVAL_MS) {
    g_last_rescan_ms = now;
    uint8_t before = 0;
    for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
      if (screen::online(static_cast<screen::ScreenId>(i))) {
        before |= (1u << i);
      }
    }
    screen::rescan();
    uint8_t after = 0;
    for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
      if (screen::online(static_cast<screen::ScreenId>(i))) {
        after |= (1u << i);
      }
    }
    if (before != after) {
      Serial.print("[rescan] screens online: ");
      for (uint8_t i = 0; i < screen::SCREEN_COUNT; ++i) {
        Serial.print(screen::online(static_cast<screen::ScreenId>(i)) ? '1' : '0');
      }
      Serial.print(" mux_err=");
      Serial.println(pca9548a::lastError());
    }
  }
}
