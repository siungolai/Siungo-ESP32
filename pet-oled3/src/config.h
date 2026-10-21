/**
 * config.h -- pet-oled3 pin and constant configuration
 *
 * This file holds ONLY pins, addresses, and timing. No secrets here.
 *
 * Pin basis: docs/adr/0006-hardware-interface-aligned-with-status-oled3.md
 *            docs/adr/0007-two-buttons-only-input.md
 * Hard constraints: HARDWARE.md section 1.2 (usable GPIO pool),
 *                   section 6.1 (GPIO 1/2 must not be used for I2C).
 *
 * The whole pin set is IDENTICAL to status-oled3/src/config.h. That is not
 * an accident of history: the 2026-10-21 decision was that the two projects
 * share one set of modules, so "copy the bench-measured constants" is the
 * rule rather than "re-derive them". Every value below carries the date it
 * was measured on, not the date it was written down.
 */
#pragma once

#include <stdint.h>

namespace config {

// ------------------------------------------------------------------ upstream I2C
// Bench wiring, measured 2026-10-17 by the status-oled3 project:
//   GPIO4 -> module SCL,  GPIO5 -> module SDA
// (operator report: "g4 connects scl; g5 connects sda").
//
// ORDER MATTERS, and it was got backwards once already, at a cost of 36
// bench hours. On the bench the wires are
//   GPIO4 -> module SCL,  GPIO5 -> module SDA
// so GPIO5 drives SDA and GPIO4 drives SCL. Measured by i2c-probe v7: the
// pair (SDA=GPIO5, SCL=GPIO4) ACKs 0x70, the pair (SDA=GPIO4, SCL=GPIO5)
// answers nothing at all -- a swapped pair cannot form a START condition,
// so every transaction returns error 2 with the lines sitting perfectly
// idle HIGH. If 0x70 ever stops answering, check the order first.
//
// HARDWARE.md section 6.1: GPIO 1/2 are forbidden (whole segment times out).
static const uint8_t I2C_SDA_PIN = 5;
static const uint8_t I2C_SCL_PIN = 4;
static const uint32_t I2C_HZ = 400000;

// Per-transaction timeout. HARDWARE.md section 6.2 measured that WITHOUT a
// timeout, one absent screen's flush round blocks for nearly 800 ms and
// wrecks the 1 s refresh cadence.
static const uint32_t I2C_TIMEOUT_MS = 50;

// ------------------------------------------------------------------ PCA9548A
// 0x70 = A0/A1/A2 all tied to GND, measured 2026-10-17 (i2c-probe v7:
// single ACK at 0x70 with SDA=GPIO5 SCL=GPIO4).
static const uint8_t PCA9548A_ADDR = 0x70;

// ------------------------------------------------------------------ screen topology
// One knob: how many physical panels are actually wired.
//
//   ACTIVE_SCREENS = 3 -> the mux is in the path, each logical screen owns
//                         a channel and a panel (docs/adr/0001)  [this rig]
//   ACTIVE_SCREENS = 1 -> no mux; all logical screens share the one panel
//                         and the buttons choose which one is shown
//
// The mux is in the signal path exactly when this count is greater than 1,
// so there is only one knob and no combination of settings that is wrong.
static const uint8_t ACTIVE_SCREENS = 3;

// ------------------------------------------------------------------ downstream screens
// All three panels keep the factory address 0x3C; address conflicts are
// isolated per channel (HARDWARE.md section 4.6).
static const uint8_t SSD1306_ADDR = 0x3C;

// Logical screens. The VALUES are the pet-oled3 role names; the role split
// itself is docs/adr/0003-screen-role-split.md:
//   ch0 = role screen   (the pet, its face, its mood)
//   ch1 = stats screen  (the three numbers)
//   ch2 = log screen    (events, menus, prompts)
//
// The enum names live in screen.h (SCREEN_ROLE / SCREEN_STATS / SCREEN_LOG),
// NOT here, because screen.h is the file that owns the screen abstraction.
// The channel mapping is here, because a role change is a config edit.
static const uint8_t SCREEN_COUNT = 3;
static const uint8_t SCREEN_CHANNEL[SCREEN_COUNT] = {0, 1, 2};

// ------------------------------------------------------------------ buttons
// Two tactile buttons, internal pull-up, pressed = LOW.
// Bench wiring measured 2026-10-16 (status-oled3 ADR-0007): button A on
// GPIO6, button B on GPIO7. The original plan (GPIO10/11/12 for A/B/buzzer)
// was superseded by the measured wiring.
//
// The CONSTANT NAMES still say PAGE / FOCUS -- that is inherited from
// status-oled3, where A flipped pages and B switched the focused screen.
// This project has no pages and no focus switching: A moves the action
// cursor (or opens the menu on a long press), B executes / confirms / backs
// out. The names are a known historical leftover; renaming them means
// touching config.h, button.h, button.cpp and main.cpp together
// (docs/adr/0007-two-buttons-only-input.md).
static const uint8_t PIN_BUTTON_PAGE = 6;   // button A: move cursor / open menu
static const uint8_t PIN_BUTTON_FOCUS = 7;  // button B: execute / confirm / back

// Debounce: sample press and release once each; a state change is accepted
// only if it holds for the whole interval. 20 ms is comfortably above the
// few-millisecond chatter of a mechanical contact.
static const uint32_t BUTTON_DEBOUNCE_MS = 20;

// Threshold at which a hold becomes a long press. This project USES long
// press (docs/adr/0009): A-long opens the menu, B-long leaves it.
static const uint32_t BUTTON_LONGPRESS_MS = 1000;

// ------------------------------------------------------------------ buzzer
// Active buzzer: sounds at a fixed frequency the moment DC is applied.
// Information can only be encoded in how long it sounds / how long it
// pauses; pitch cannot be changed (docs/adr/0003).
//
// Pin: GPIO21, bench wiring measured 2026-10-16 (status-oled3 ADR-0007).
// The original plan said GPIO10/11/12; GPIO21 is the measured answer.
//
// Note on GPIO12: an earlier draft of this comment said GPIO12 was dropped
// because it is an "MTDI strapping pin". That was wrong -- datasheet v2.2
// section 2.3.4 / section 3 put the strapping pins at GPIO 0/3/45/46, and
// MTDI is GPIO 41. GPIO12 is an ordinary P2 IO. The pin moved to 21
// because the bench wiring says 21, not because 12 was forbidden.
static const uint8_t PIN_BUZZER = 21;

static const bool BUZZER_ACTIVE_HIGH = true;  // HIGH sounds
static const uint32_t BUZZER_BEEP_MS = 80;     // short beep
static const uint32_t BUZZER_LONG_MS = 400;    // long beep
static const uint32_t BUZZER_GAP_MS = 120;     // pause between repeated beeps

// The very short click for a plain key press. 20 ms is at the edge of
// audibility on a piezo element, which is the point: it acknowledges the
// key without adding to the noise of the room.
static const uint32_t BUZZER_CLICK_MS = 20;

// Drive strength for the buzzer pin. The buzzer is driven as a SOURCE
// (GPIO21 -> buzzer + terminal, buzzer - terminal -> GND), so what matters
// is the high-level source current I_OH.
//
// ESP32-S3 datasheet v2.2 section 2.3.4 note 5 gives the DEFAULT drive
// strength per pin group: GPIO17/18 = 10 mA, GPIO19/20 = 40 mA, all other
// pins = 20 mA. GPIO21 falls in the "all other" group, so its default is
// 20 mA -- which is right at the bottom of the 20-30 mA range a typical
// active buzzer draws. The IO electrical characteristics table gives
// I_OH = 40 mA when PAD_DRIVER = 3, so the pin must be raised to the
// strongest drive capability explicitly.
//
// Arduino's pinMode() does NOT do this: __pinMode() in the core only calls
// gpio_config() and never touches the pad driver register. So the driver
// has to call gpio_set_drive_capability() itself.
//
// See docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md for
// the full evidence chain and the transistor fallback.
static const uint8_t BUZZER_DRIVE_CAP = 3;  // GPIO_DRIVE_CAP_3 = strongest

// ------------------------------------------------------------------ refresh cadence
// The game tick runs at 1 Hz. The role screen redraws on the same cadence
// (its expression changes with the numbers); the stats screen redraws with
// it; the log screen redraws only when something is logged.
static const uint32_t TICK_INTERVAL_MS = 1000;

// How often an absent screen is re-probed, so unplug/replug recovers
// without a reset. Only currently-offline channels are probed, so the
// steady state costs nothing.
static const uint32_t RESCAN_INTERVAL_MS = 5000;

// NOTE: `status-oled3/src/config.h` has a `LOG_LINE_MAX` (160) for its health
// summaries, which build a long line before printing it. This project has no
// such path: the log ring in main.cpp caps each line at `kLogTextLen` (22,
// exactly what the 128 px panel can show) and the serial copy prints the same
// string. So there is deliberately no LOG_LINE_MAX here -- a copy of the
// constant would be dead weight and would invite the wrong assumption that
// lines can be 160 chars long.

}  // namespace config
