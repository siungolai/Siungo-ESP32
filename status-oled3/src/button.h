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
 * Only short presses are used at M0 (page flip / focus switch). The long
 * press threshold is kept in config.h so M2 can add long press without
 * restructuring this file.
 */
#pragma once

#include <stdint.h>

namespace button {

// Which physical button.
enum ButtonId : uint8_t {
  BTN_PAGE = 0,   // button A: page flip on the focused screen
  BTN_FOCUS = 1,  // button B: switch the focused screen
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
Event drain(ButtonId id);

// Instantaneous pin level (true = pressed), for serial diagnostics.
bool pressed(ButtonId id);

// Milliseconds the button has currently been held; 0 when not pressed.
uint32_t heldMs(ButtonId id);

}  // namespace button
