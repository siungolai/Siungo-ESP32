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

// The logical screen currently taking over the shared panel. Only meaningful
// in the single-screen topology; with one panel per screen it is simply the
// screen the user last focused and has no visible effect.
ScreenId g_focused = SCREEN_ROLE;

// The driver instances. Static storage, so no allocation ever happens on the
// heap after boot. All share the same pins and the same 0x3C address; the
// only difference between them is which mux channel gets selected before
// they touch the bus (see channelOf()), and in the single-screen topology
// no channel is selected at all because the panel is on the bus directly.
// The Ssd1306I2c constructor does not touch the bus, so building three is
// free.
Ssd1306I2c g_drivers[SCREEN_COUNT] = {
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
    Ssd1306I2c(config::I2C_SCL_PIN, config::I2C_SDA_PIN, config::SSD1306_ADDR,
               config::I2C_HZ),
};

// Mux channel for a screen id. config::SCREEN_CHANNEL[] lives in config.h so
// a role change (e.g. swapping stats and log) is a one-line config edit.
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

// Bring up the panel in the single-screen topology: no channel to select,
// because the panel hangs off the upstream pins directly.
bool bringUpDirect() { return g_drivers[0].begin(); }

// Bring the given screen's driver up on its own channel.
// Selects and deselects the channel itself; caller must not hold it.
bool bringUp(ScreenId id) {
  if (pca9548a::isDirect()) {
    return bringUpDirect();  // no mux: the panel is on the bus already
  }
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
  //
  // Single-screen topology: the mux is not in the path, so begin() is only
  // needed for the bus initialisation it performs (Wire.begin + timeout).
  // pca9548a::begin() returns false there, and that is not a fault.
  pca9548a::begin();

  if (pca9548a::isDirect()) {
    // One probe covers all three logical screens: they share the one panel.
    const bool ok = bringUpDirect();
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
      g_screens[i].oled = &g_drivers[i];
      g_screens[i].online = ok;
      g_screens[i].dirty = true;
      g_screens[i].page.index = 0;
      g_screens[i].page.count = 1;
    }
    return;
  }

  for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
    g_screens[i].oled = &g_drivers[i];
    g_screens[i].online = bringUp(static_cast<ScreenId>(i));
    g_screens[i].dirty = true;
    g_screens[i].page.index = 0;
    g_screens[i].page.count = 1;
  }
}

// Record which logical screen owns the shared panel (single-screen topology).
// Called by main.cpp when focus changes, so the next render paints it.
void setFocused(ScreenId id) {
  if (!validId(id)) {
    return;
  }
  g_focused = id;
}

void rescan() {
  // Only channels previously reported offline are re-probed, so the steady
  // state costs zero upstream traffic. A screen that just got plugged in is
  // picked up within one rescan cycle.
  //
  // Single-screen topology: all three logical screens map onto the ONE panel,
  // so one probe covers all of them. There is nothing else to wait for, and
  // re-probing the same channel three times would be three identical
  // transactions that can only disagree with each other.
  if (pca9548a::isDirect()) {
    const bool ok = bringUpDirect();
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
      if (g_screens[i].online == ok) {
        continue;
      }
      g_screens[i].online = ok;
      if (ok) {
        g_screens[i].dirty = true;  // it is back: redraw it fully
      }
    }
    return;
  }

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

bool showable(ScreenId id) {
  if (!validId(id)) {
    return false;
  }
  if (!g_screens[static_cast<uint8_t>(id)].online) {
    return false;  // absent panel: nothing to paint on
  }
  // One panel cannot show three screens at once. Only the focused one is
  // showable, and the caller repaints it by invalidating it on focus change
  // (main.cpp onButtonFocus()).
  if (PHYSICAL_COUNT <= 1) {
    return id == g_focused;
  }
  return true;
}

// Deprecated and uncalled: focus changes go through setFocused() plus
// invalidate() on every screen, which main.cpp owns. It is kept only because
// removing a public symbol is a wider change than this review is allowed to
// make, and it is documented here so the next reader does not have to grep
// for callers to find out. Do not call it: it sets one dirty flag and the
// comment above it used to promise behaviour the body never had (marking the
// other screens clean), which is exactly the kind of divergence that costs a
// debugging session. Single entry point for focus changes is setFocused().
void showFocused() {
  if (PHYSICAL_COUNT > 1) {
    return;
  }
  if (g_focused >= SCREEN_COUNT) {
    return;
  }
  if (!g_screens[g_focused].online) {
    return;
  }
  g_screens[g_focused].dirty = true;
}

bool online(ScreenId id) {
  if (!validId(id)) {
    return false;
  }
  return g_screens[static_cast<uint8_t>(id)].online;
}

// Two clearing paths exist on purpose and they are not interchangeable:
//   clear()            -- public, offline-gated, marks the screen dirty
//   s.oled->clear()    -- framebuffer only, no bus transaction, no gate
// drawTitleBar() uses the second one because it runs AFTER select(), where
// taking the public one would deselect the channel it is about to draw on
// (and, in the single-panel topology, double-book the panel). If you are
// adding a third caller, pick deliberately.
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
  // Clear BEFORE the mux select, and unconditionally.
  //
  // clear() only zeroes this screen's own framebuffer; it touches no bus, so
  // it has no business being behind an I2C gate. It used to sit after the
  // select() early-return, which produced a double-exposure bug on page flips
  // (reported 2026-10-17): a transient mux NACK made drawTitleBar() bail out
  // before clearing, while the printKV() calls that follow each re-select the
  // channel and can succeed. drawText5x7() ORs glyph pixels and never erases
  // ("off pixels do not overwrite what is already there"), so the new page was
  // painted on top of the old page's framebuffer. Two screens of text, one
  // panel.
  //
  // The invariant worth keeping: if anything of this frame is drawn, the
  // framebuffer it lands on started from zero.
  s.oled->clear();
  if (!pca9548a::select(channelOf(id))) {
    // Framebuffer is cleared and the body rows below still select and write
    // their own text, so letting them paint is safe: at worst the title bar
    // is missing for one frame, and the next tick redraws it.
    return;
  }
  // Row 0 is the title bar. Focused screens draw it inverted (all pixels on
  // with the text knocked out), the cheapest "this screen is focused" marker
  // that survives a glance from across the desk.
  //
  // Order matters: draw the text FIRST, then invert the whole row. Inverting
  // first and drawing the text second would paint the glyph pixels ON, which
  // is exactly the "inverted bar, invisible title" bug seen on the bench
  // (2026-10-17). drawText5x7() overwrites its glyph columns and clears the
  // inter-character gap, so it must not run over an inverted background.
  s.oled->drawText5x7(2, 0, title);
  if (focused) {
    s.oled->invertPageSpan(0, OLED_WIDTH - 1, 0);
  }
  pca9548a::deselectAll();
  s.dirty = true;
}

void drawBitmap(ScreenId id, uint8_t x, uint8_t y, const uint8_t* cols,
                uint8_t n, uint8_t rowsHeight) {
  if (!validId(id) || cols == nullptr) {
    return;
  }
  // rowsHeight is 8 or 16; anything else is a caller bug, and the bit
  // arithmetic below silently misbehaves for other values, so it is
  // rejected here rather than drawn wrong.
  if (rowsHeight != 8 && rowsHeight != 16) {
    return;
  }
  ScreenEntry& s = g_screens[static_cast<uint8_t>(id)];
  if (!s.online) {
    return;
  }
  if (!pca9548a::select(channelOf(id))) {
    return;
  }
  s.oled->drawBits(x, y, cols, n, rowsHeight);
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
