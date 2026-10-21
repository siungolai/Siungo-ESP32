/**
 * button.h -- tactile button reading with software debouncing
 *
 * Two buttons, internal pull-up, pressed = LOW (see docs/adr/0007).
 *
 * The debounce algorithm is a 4-state machine advanced by tick() calls:
 *
 *   idle --press--> debounce_down --held--> down
 *                            \--release--> idle          (rejected chatter)
 *   down --release--> debounce_up --held--> idle  + emit event
 *                                \--press--> down       (rejected chatter)
 *
 * "held for the whole interval" is what filters mechanical chatter: a real
 * contact closes for tens of milliseconds, bounce spikes for microseconds
 * to a few milliseconds, so a 20 ms window rejects them all. There is no
 * external RC filter; the whole job is done in software so that swapping a
 * switch does not require changing the wiring.
 *
 * Why a state machine instead of "read and delay(20)": delay() would stall
 * the 1 s refresh and the 15 s poll. tick() is called every loop() and
 * returns immediately, so the rest of the program keeps its cadence.
 *
 * Event-per-press contract, which the implementation must uphold even though
 * it is not visible in the state diagram above:
 *   - a SHORT press emits exactly one EVENT_SHORT_PRESS, on release;
 *   - a LONG press emits exactly one EVENT_LONG_PRESS, the moment the
 *     threshold is crossed, and nothing on release.
 * A held button therefore reports one event per physical press, never one
 * per tick(). See long_fired in button.cpp: the bounce path
 * (down -> debounce_up -> down) re-enters ST_DOWN with press_ms unchanged,
 * so the threshold is crossed again on the very next tick and a naive
 * implementation re-emits the long press every iteration.
 *
 * Only short presses are used at M0 (move the cursor / execute). The long
 * press threshold is kept in config.h so M1 can add long press without
 * restructuring this file.
 */
#pragma once

#include <stdint.h>

namespace button {

// Which physical button.
//
// The NAMES are inherited from status-oled3, where A flipped pages and B
// switched the focused screen. In this project the same two pins carry
// different actions: A moves the action cursor (long press opens the menu),
// B executes / confirms / backs out (docs/adr/0009-two-button-interaction-coverage.md).
// Renaming BTN_A / BTN_B means changing button.h, button.cpp, config.h and
// main.cpp together -- do not rename one of them.
enum ButtonId : uint8_t {
  BTN_PAGE = 0,   // button A: move the action cursor (long press: open menu)
  BTN_FOCUS = 1,  // button B: execute / confirm (long press: back)
};

// What a button did since the last drain().
enum Event : uint8_t {
  EVENT_NONE = 0,
  EVENT_SHORT_PRESS = 1,  // press -> release inside the long-press threshold
  EVENT_LONG_PRESS = 2,   // press held past BUTTON_LONGPRESS_MS
};

// Configure the pins. Call once from setup(), before tick().
void begin();

// Advance the debounce state machine. Call every loop() iteration.
// Does not block.
void tick();

// Collect the pending event for one button and clear it.
// Returns EVENT_NONE when nothing happened.
// Reading twice returns EVENT_NONE the second time, so a missed loop()
// iteration can never fire the same event twice.
//
// The "one event per press" contract (see the header comment) holds whether
// or not the event is drained: drain() clears pending but leaves the
// long-press latch alone, so releasing a long press after a drain still
// does not produce a short press.
Event drain(ButtonId id);

// Instantaneous pin level (true = pressed), for serial diagnostics.
// UNUSED by the firmware; see the note at the bottom of this block.
bool pressed(ButtonId id);

// Milliseconds the button has currently been held; 0 when not pressed.
// UNUSED by the firmware: the long-press threshold is handled inside tick()
// and surfaced as EVENT_LONG_PRESS, so no caller needs to measure it again.
uint32_t heldMs(ButtonId id);

}  // namespace button
