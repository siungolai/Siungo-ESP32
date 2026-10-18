/**
 * button.cpp -- debounce state machine implementation
 *
 * All timing is non-blocking: tick() only compares millis() against stored
 * timestamps. loop() calls it as often as it likes.
 */
#include "button.h"

#include <Arduino.h>

#include "config.h"

namespace button {

namespace {

// How many buttons the machine tracks. Indexing is by ButtonId.
constexpr uint8_t kButtonCount = 2;

constexpr uint8_t kPin[kButtonCount] = {
    config::PIN_BUTTON_PAGE,   // BTN_PAGE
    config::PIN_BUTTON_FOCUS,  // BTN_FOCUS
};

// Physical level of the pin when the button is held.
// Internal pull-up, press shorts the pin to GND, so pressed = LOW.
constexpr int kPressedLevel = LOW;

// Internal states of the 4-state machine documented in button.h.
enum State : uint8_t {
  ST_IDLE = 0,
  ST_DEBOUNCE_DOWN = 1,
  ST_DOWN = 2,
  ST_DEBOUNCE_UP = 3,
};

struct ButtonEntry {
  State state = ST_IDLE;
  uint32_t change_ms = 0;  // millis() when the level last changed
  uint32_t press_ms = 0;   // millis() when the debounce that led here started
  // One long-press event per physical press. Cleared when the press ends
  // (ST_IDLE), set the moment the threshold is crossed. Without it the
  // ST_DOWN -> ST_DEBOUNCE_UP -> ST_DOWN bounce path re-enters ST_DOWN with
  // press_ms unchanged, so the very next tick() satisfies the long-press
  // threshold again and a held button emits EVENT_LONG_PRESS every ~20 ms.
  bool long_fired = false;
  Event pending = EVENT_NONE;
  bool stable_level = false;  // debounced level; false = released (pull-up)
};

ButtonEntry g_btn[kButtonCount];

bool validId(ButtonId id) {
  return static_cast<uint8_t>(id) < kButtonCount;
}

}  // namespace

void begin() {
  for (uint8_t i = 0; i < kButtonCount; ++i) {
    pinMode(kPin[i], INPUT_PULLUP);
    ButtonEntry& b = g_btn[i];
    b.state = ST_IDLE;
    b.change_ms = 0;
    b.press_ms = 0;
    b.pending = EVENT_NONE;
    b.long_fired = false;
    // Seed the debounced level with what the pin actually reads, so a button
    // held while the firmware boots is not seen as a fresh press.
    b.stable_level = (digitalRead(kPin[i]) == kPressedLevel);
  }
}

void tick() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < kButtonCount; ++i) {
    ButtonEntry& b = g_btn[i];
    const bool raw_pressed = (digitalRead(kPin[i]) == kPressedLevel);

    switch (b.state) {
      case ST_IDLE:
        if (raw_pressed) {
          b.state = ST_DEBOUNCE_DOWN;
          b.change_ms = now;
          // A new press is a fresh chance at a long press.
          b.long_fired = false;
        }
        break;

      case ST_DEBOUNCE_DOWN:
        // The level must stay stable for the whole debounce window. A bounce
        // back to released inside the window restarts the window.
        if (!raw_pressed) {
          b.state = ST_IDLE;
          b.change_ms = now;
        } else if (now - b.change_ms >= config::BUTTON_DEBOUNCE_MS) {
          b.state = ST_DOWN;
          b.press_ms = now;
          b.stable_level = true;
        }
        break;

      case ST_DOWN:
        if (!raw_pressed) {
          b.state = ST_DEBOUNCE_UP;
          b.change_ms = now;
        } else if (now - b.press_ms >= config::BUTTON_LONGPRESS_MS) {
          // Long press fires the moment the threshold is crossed, while the
          // button is still held, so the release afterwards does NOT also
          // produce a short press.
          //
          // long_fired latches the event to THIS press. A bounce back down
          // (ST_DEBOUNCE_UP -> ST_DOWN) keeps press_ms, so without the latch
          // the threshold is still crossed on the next tick and the event
          // fires again -- once per loop() iteration for as long as the
          // button is held.
          if (!b.long_fired) {
            b.long_fired = true;
            b.pending = EVENT_LONG_PRESS;
          }
          b.state = ST_DEBOUNCE_UP;
          b.change_ms = now;
        }
        break;

      case ST_DEBOUNCE_UP:
        if (raw_pressed) {
          b.state = ST_DOWN;  // bounce back down: still the same press
        } else if (now - b.change_ms >= config::BUTTON_DEBOUNCE_MS) {
          // Released and stable. The press only becomes a short press when
          // it never reached the long-press threshold.
          //
          // pending is read through long_fired, not directly: drain() clears
          // pending between the two ticks of this branch, and a drain() that
          // raced the release would otherwise lose the "was a long press"
          // fact and misreport the release as a short press.
          if (!b.long_fired) {
            b.pending = EVENT_SHORT_PRESS;
          }
          b.long_fired = false;
          b.stable_level = false;
          b.state = ST_IDLE;
        }
        break;
    }
  }
}

Event drain(ButtonId id) {
  if (!validId(id)) {
    return EVENT_NONE;
  }
  Event& slot = g_btn[static_cast<uint8_t>(id)].pending;
  const Event out = slot;
  slot = EVENT_NONE;
  return out;
}

bool pressed(ButtonId id) {
  if (!validId(id)) {
    return false;
  }
  return g_btn[static_cast<uint8_t>(id)].stable_level;
}

uint32_t heldMs(ButtonId id) {
  if (!validId(id)) {
    return 0;
  }
  const ButtonEntry& b = g_btn[static_cast<uint8_t>(id)];
  if (b.state == ST_IDLE) {
    return 0;
  }
  // Unsigned subtraction: correct across the millis() wrap, and the value is
  // only ever compared against a threshold, never against 0.
  return millis() - b.press_ms;
}

}  // namespace button
