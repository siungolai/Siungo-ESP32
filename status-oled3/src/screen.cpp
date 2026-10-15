/**
 * screen.cpp -- screen abstraction implementation
 *
 * One ScreenEntry per logical screen. Each entry owns an Ssd1306I2c driver
 * instance (1 KB framebuffer each) plus the online/dirty/page state.
 *
 * The one rule every function must keep: the mux is selected exactly for the
 * duration of one screen's work, then deselected. pca9548a.select() already
 * no-ops on a re-select of the same channel, so a screen that draws and
 * flushes costs one upstream transaction for the mux, not one per page.
 */
#include "screen.h"

#include <Wire.h>

#include <cstdio>

#include "config.h"
#include "pca9548a.h"

namespace screen {

namespace {

// Per-screen state. The driver is a pointer because Ssd1306I2c has no default
// constructor: it must be built with the pins, and a static array of values
// would need them repeated at namespace scope. begin() points each entry at
// its driver once; every other function just dereferences. nullptr means
// "begin() has not run yet", which no caller can reach because every public
// entry point checks online() first.
struct ScreenEntry {
  Ssd1306I2c* oled = nullptr;
  bool online = false;
  bool dirty = true;   // start dirty: first update() must draw something
  PageState page = {0, 1};
};

ScreenEntry g_screens[SCREEN_COUNT];

// The three driver instances. Static storage, so no allocation ever happens
// on the heap after boot. All three share the same pins and the same 0x3C
// address; the only difference between them is which mux channel gets
// selected before they touch the bus (see channelOf()). The Ssd1306I2c
// constructor does not touch the bus, so building three is free.
Ssd1306I2c g_drivers[SCREEN_COUNT] = {
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
};

// Mux channel for a screen id. config::SCREEN_CHANNEL[] lives in config.h so
// a role change (e.g. swapping server and mailbox) is a one-line config edit.
uint8_t channelOf(ScreenId id) {
  if (static_cast<uint8_t>(id) >= SCREEN_COUNT) {
    return 0;  // defensive: callers only pass valid ids, but a bad index must
               // not read past the array
  }
  return config::SCREEN_CHANNEL[static_cast<uint8_t>(id)];
}

// Guard against invalid ids in the public API.
bool validId(ScreenId id) {
  return static_cast<uint8_t>(id) < SCREEN_COUNT;
}

// Bring the given screen's driver up on its own channel.
// Selects and deselects the channel itself; caller must not hold it.
bool bringUp(ScreenId id) {
  const uint8_t ch = channelOf(id);
  if (!pca9548a::select(ch)) {
    return false;  // mux offline (or write failed): nothing downstream works
  }
  const bool ok = g_drivers[static_cast<uint8_t>(id)].begin();
  pca9548a::deselectAll();
  return ok;
}

}  // namespace

void begin() {
  // The mux must come up first: every screen probe rides it.
  // When the mux itself is absent, all three screens stay offline, and the
  // serial report below is the whole diagnosis.
  pca9548a::begin();

  for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
    g_screens[i].oled = &g_drivers[i];
    g_screens[i].online = bringUp(static_cast<ScreenId>(i));
    g_screens[i].dirty = true;
    g_screens[i].page.index = 0;
    g_screens[i].page.count = 1;
  }
}

void rescan() {
  // Only channels previously reported offline are re-probed, so the steady
  // state costs zero upstream traffic. A screen that just got plugged in is
  // picked up within one rescan cycle.
  for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
    if (g_screens[i].online) {
      continue;
    }
    g_screens[i].online = bringUp(static_cast<ScreenId>(i));
    if (g_screens[i].online) {
      g_screens[i].dirty = true;  // it is back: redraw it fully
    }
  }
}

bool online(ScreenId id) {
  if (!validId(id)) {
    return false;
  }
  return g_screens[static_cast<uint8_t>(id)].online;
}

void clear(ScreenId id) {
  if (!validId(id)) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  if (!s.online) {
    return;  // offline: framebuffer content is irrelevant, skip the work
  }
  if (!pca9548a::select(channelOf(id))) {
    return;
  }
  s.oled->clear();
  pca9548a::deselectAll();
  s.dirty = true;
}

void printLine(ScreenId id, uint8_t row, const char* text) {
  if (!validId(id) || text == nullptr) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  if (!s.online) {
    return;
  }
  if (!pca9548a::select(channelOf(id))) {
    return;
  }
  // 7 text rows fit in 64 pixels (8 px each). Row 0 is reserved for the
  // title bar, so the body range is 1..7; clamp anything larger.
  if (row < 1) {
    row = 1;
  }
  if (row > 7) {
    row = 7;
  }
  s.oled->drawText5x7(0, row, text);
  pca9548a::deselectAll();
  s.dirty = true;
}

void printKV(ScreenId id, uint8_t row, const char* key, const char* value) {
  if (key == nullptr || value == nullptr) {
    return;
  }
  // One shared buffer: screen.cpp is the only writer and no caller holds a
  // reference across the call. 32 bytes is enough for "k=v" on a 128 px
  // screen (21 chars at 6 px each).
  char line[32];
  snprintf(line, sizeof(line), "%s=%s", key, value);
  printLine(id, row, line);
}

void drawTitleBar(ScreenId id, const char* title, bool focused) {
  if (!validId(id) || title == nullptr) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  if (!s.online) {
    return;
  }
  if (!pca9548a::select(channelOf(id))) {
    return;
  }
  // Row 0 is the title bar. Focused screens draw it inverted (all pixels on
  // with the text knocked out), the cheapest "this screen is focused" marker
  // that survives a glance from across the desk.
  s.oled->clear();
  if (focused) {
    s.oled->fillPageSpan(0, OLED_WIDTH - 1, 0, 0xFF);
  }
  s.oled->drawText5x7(2, 0, title);
  pca9548a::deselectAll();
  s.dirty = true;
}

void invalidate(ScreenId id) {
  if (!validId(id)) {
    return;
  }
  g_screens[static_cast<uint8_t>(id)].dirty = true;
}

bool isDirty(ScreenId id) {
  if (!validId(id)) {
    return false;
  }
  return g_screens[static_cast<uint8_t>(id)].dirty;
}

void flush(ScreenId id) {
  if (!validId(id)) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  if (!s.online) {
    return;  // Screen Absent: no I2C traffic at all (HARDWARE.md 6.2)
  }
  if (!pca9548a::select(channelOf(id))) {
    return;
  }
  s.oled->flush();
  pca9548a::deselectAll();
  s.dirty = false;
}

void update() {
  // Redraw every screen that is dirty and online. Drawing happens in the
  // caller's draw* functions (which call the primitives above); this layer
  // only owns the "when do we push" decision, so a 1 s tick with nothing
  // changed costs no bus traffic.
  for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
    const ScreenId id = static_cast<ScreenId>(i);
    if (!g_screens[i].online || !g_screens[i].dirty) {
      continue;
    }
    // The caller's draw functions select the channel themselves, so all
    // that is left here is the push.
    flush(id);
  }
}

void setPageCount(ScreenId id, uint8_t count) {
  if (!validId(id)) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  // count < 1 is not a usable state: a screen with no pages would show
  // nothing, so clamp to 1 instead of trusting the caller.
  s.page.count = (count < 1) ? 1 : count;
  if (s.page.index >= s.page.count) {
    s.page.index = static_cast<uint8_t>(s.page.count - 1);
  }
}

uint8_t pageIndex(ScreenId id) {
  if (!validId(id)) {
    return 0;
  }
  return g_screens[static_cast<uint8_t>(id)].page.index;
}

uint8_t pageCount(ScreenId id) {
  if (!validId(id)) {
    return 1;
  }
  return g_screens[static_cast<uint8_t>(id)].page.count;
}

void nextPage(ScreenId id) {
  if (!validId(id)) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  s.page.index = static_cast<uint8_t>((s.page.index + 1) % s.page.count);
  s.dirty = true;
}

}  // namespace screen
