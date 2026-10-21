/**
 * screen.h -- screen abstraction layer (N channels -> 3 logical screens)
 *
 * Responsibilities:
 *   1. Wrap the SSD1306 driver with pca9548a.select(channel) so callers never
 *      touch the mux.
 *   2. Cache "is this screen online" (Screen Absent per GLOSSARY.md) and have
 *      flush() return immediately while offline, instead of blocking for
 *      50 ms x 8 transactions per round (HARDWARE.md section 6.2).
 *   3. Provide text primitives: clear/draw on the current page, flip to the
 *      next page, expose a dirty flag so loop() only redraws what changed.
 *
 * Screens are stateless apart from the framebuffer: the pages are defined by
 * the callers in drawRoleScreen(), drawStatsScreen(), drawLogScreen() (in
 * src/main.cpp). This layer only offers "current page" and "page count", and
 * knows nothing about content.
 */
#pragma once

#include <stdint.h>

#include "config.h"
#include "pca9548a.h"
#include "ssd1306_i2c.h"

namespace screen {

// Logical screens, index = array order. The channel of each screen comes from
// config.h SCREEN_CHANNEL[]; a role change is a config edit, not a code edit.
//
// The names are the pet-oled3 ROLES, per docs/adr/0003-screen-role-split.md
// and CONTEXT.md:
//   ch0 = role screen   -- the pet's face and expression
//   ch1 = stats screen  -- the three numbers
//   ch2 = log screen    -- events, menus, prompts
//
// They were renamed from status-oled3's SCREEN_LOCAL / SCREEN_SERVER /
// SCREEN_MAILBOX, which carry a status-display meaning that does not exist
// here. The VALUES 0/1/2 are unchanged, so the channel mapping still holds
// one-to-one.
enum ScreenId : uint8_t {
  SCREEN_ROLE = 0,   // the pet
  SCREEN_STATS = 1,  // the three numbers
  SCREEN_LOG = 2,    // events and menus
};

// How many logical screens exist. Used by loops that iterate all of them.
constexpr uint8_t SCREEN_COUNT = 3;

// Physical panels actually wired (config::ACTIVE_SCREENS).
//   > 1 -> mux in the signal path; every logical screen owns its own panel
//          and a channel, and all three are painted at once (docs/adr/0001)
//   1   -> no mux; all logical screens share ONE panel and the caller paints
//          only the focused one. showable() expresses that.
constexpr uint8_t PHYSICAL_COUNT = config::ACTIVE_SCREENS;

// Whether this screen is currently visible. With three panels every screen is
// visible; with one panel only the focused one is. Callers must use this
// instead of online() when deciding what to paint.
//
// Note that "visible" is a strictly stronger condition than "online" here: in
// the single-panel topology a dirty non-focused screen is online but must NOT
// be painted, because the panel it would land on is showing the focused one.
bool showable(ScreenId id);

// Paint the focused screen onto the shared panel (single-screen topology).
// No-op when PHYSICAL_COUNT > 1: each screen already owns a panel.
//
// UNUSED: focus changes go through setFocused() + invalidate() in main.cpp.
// Kept as a declared-but-uncalled symbol on purpose (see the note in the .cpp)
// rather than deleted, because deleting a public symbol is outside the scope
// of the change that deprecated it.
void showFocused();

// Current page of one screen. A page is one "screenful" of content; pages of
// the same screen show the same kind of information (GLOSSARY.md "page").
struct PageState {
  uint8_t index;      // current page, 0-based
  uint8_t count;      // total page count, >= 1
};

// One-time setup: probe all channels, record online state.
// Does not draw anything; draw() does the drawing.
void begin();

// Force a re-probe of all three screens (e.g. after a re-plug).
// Only re-probes channels whose previous result was offline, so the steady
// state costs no upstream traffic.
void rescan();

// Whether the given screen is online (probed + acked at least once).
bool online(ScreenId id);

// Flush this screen's framebuffer out.
// Offline -> returns immediately without touching the bus (HARDWARE.md
// section 6.2: must not block on an absent screen).
// Online -> selects the channel, pushes the buffer, marks it clean.
void flush(ScreenId id);

// Mark this screen dirty so the next update() redraws it.
void invalidate(ScreenId id);

// Whether this screen is currently dirty (waiting for a redraw).
// Exists so main.cpp can skip a screen in one pass instead of relying on
// flush() to silently drop it.
bool isDirty(ScreenId id);

// Redraw all screens that are dirty and online.
// Each screen's draw happens in the draw* callback supplied by the caller,
// so the "what does this screen show" knowledge lives outside this layer.
//
// UNUSED by main.cpp, which runs its own dispatch (renderScreen() in
// src/main.cpp) because it has to draw the CONTENT and then flush, not just
// flush. Kept for the same reason as showFocused(): removing a public symbol
// is wider than this change. If it is ever called again, note that its gate
// is online() while main.cpp's is showable() -- in the single-panel topology
// that difference pushes a non-focused screen onto the shared panel.
void update();

// Text primitive: clear the whole framebuffer and move the cursor home.
void clear(ScreenId id);

// Print one line at row (0-7, one 8-pixel row per line). out-of-range row
// falls back to the last row rather than corrupting memory.
void printLine(ScreenId id, uint8_t row, const char* text);

// Print "label: value" in one call; a colon and one space go in between.
// Shortens the many duplicated sprintf calls in the draw functions.
void printKV(ScreenId id, uint8_t row, const char* key, const char* value);

// Draw a right-aligned summary in the top status bar: focus marker + title +
// online/offline mark. The focused screen draws an inverted title bar.
void drawTitleBar(ScreenId id, const char* title, bool focused);

// Raw bitmap blit onto the screen's framebuffer.
//
// Same coordinate model as Ssd1306I2c::drawBits: one byte = one column of 8
// vertical pixels, bit0 = the top row within the page; with rowsHeight = 16
// the low byte comes first and the high byte second (the Chinese glyph
// table's layout).
//
// This is the ONE way to put non-text pixels on a panel, and it exists
// because the pet's face is not text. It deliberately does not hand out the
// framebuffer: the bitmap is copied in through the same online gate and the
// same select/deselect discipline as every other primitive, so the
// "one channel selected at a time" rule cannot be bypassed by drawing.
void drawBitmap(ScreenId id, uint8_t x, uint8_t y, const uint8_t* cols,
                uint8_t n, uint8_t rowsHeight);

// ---- page state -----------------------------------------------------------
// These are pure state; the caller decides when to flip.
void setPageCount(ScreenId id, uint8_t count);
uint8_t pageIndex(ScreenId id);
uint8_t pageCount(ScreenId id);
// Advance by one, wrapping to 0 when it passes the last page.
// count < 1 is treated as 1 (a screen with no pages is not a usable state).
void nextPage(ScreenId id);

// Which logical screen owns the shared panel in the single-screen topology.
// main.cpp calls this whenever focus changes so the panel repaints.
void setFocused(ScreenId id);

// The title-bar inversion for the focused screen goes through
// Ssd1306I2c::invertPageSpan: drawTitleBar() draws the title first and inverts
// the row afterwards, so the glyph pixels end up dark on a lit bar. Filling the
// row with 0xFF and then drawing the text (the original mistake) leaves the
// text invisible. There is no raw framebuffer accessor on purpose: it would
// let a caller draw outside the online/dirty bookkeeping.

}  // namespace screen
