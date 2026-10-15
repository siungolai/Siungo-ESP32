/**
 * config.h -- status-oled3 pin and constant configuration
 *
 * This file holds ONLY pins, addresses, and timing. No secrets here.
 * The WiFi password and the aggregator X-Monitor-Key are secrets and
 * go into src/secrets.h at M1 (already covered by the repo-root
 * .gitignore rule for src/secrets.h, so they never get committed);
 * precedent: the clock-oled project's src/secrets.h.
 *
 * Pin basis: docs/adr/0007-gpio-assignment-buttons-and-buzzer.md
 * Hard constraints: HARDWARE.md section 1.2 (usable GPIO pool) and
 * section 6.1 (GPIO 1/2 must not be used for I2C).
 */
#pragma once

#include <stdint.h>

namespace config {

// ------------------------------------------------------------------ upstream I2C
// Same wiring convention as pet-oled3: SDA=GPIO8 / SCL=GPIO9.
// Both pins are marked "measured working" in HARDWARE.md section 1.2;
// uptime-oled's GPIO4/5 is a second sample of the same conclusion.
// GPIO 1/2 are forbidden (HARDWARE.md section 6.1: whole segment times out).
static const uint8_t I2C_SDA_PIN = 8;
static const uint8_t I2C_SCL_PIN = 9;
static const uint32_t I2C_HZ = 400000;

// Per-transaction timeout. HARDWARE.md section 6.2 measured that WITHOUT a
// timeout, one absent screen's flush round blocks for nearly 800 ms and
// wrecks the 1 s refresh cadence.
static const uint32_t I2C_TIMEOUT_MS = 50;

// ------------------------------------------------------------------ PCA9548A
// 0x70 = A0/A1/A2 all tied to GND. Finished modules may have the address
// jumpers soldered in place, so bringup step 1 must scan the bus for the
// real address (HARDWARE.md section 4.8 item 1). Do not hardcode blindly.
static const uint8_t PCA9548A_ADDR = 0x70;

// ------------------------------------------------------------------ downstream screens
// Role split per docs/adr/0001: ch0 = ESP32 status, ch1 = server, ch2 = mailbox.
// All three screens keep the 0x3C factory address (address conflicts are
// isolated per channel -- HARDWARE.md section 4.6).
static const uint8_t SSD1306_ADDR = 0x3C;

// Screen count and the channel of each screen. Iterating the three screens
// only ever references these two arrays.
static const uint8_t SCREEN_COUNT = 3;
static const uint8_t SCREEN_CHANNEL[SCREEN_COUNT] = {0, 1, 2};

// ------------------------------------------------------------------ buttons
// Two tactile buttons, internal pull-up, pressed = LOW.
// GPIO 10/11/12 are three consecutive free pins (HARDWARE.md section 1.2
// usable pool). Rationale for consecutive pins and the rejected
// alternatives: docs/adr/0007.
static const uint8_t PIN_BUTTON_PAGE = 10;    // button A: page on focused screen
static const uint8_t PIN_BUTTON_FOCUS = 11;   // button B: switch focused screen
static const uint8_t PIN_BUZZER = 12;         // active buzzer: on/off encoding

// Debounce: sample press and release once each; a state change is accepted
// only if it holds for the whole interval. Mechanical contacts chatter for
// a few milliseconds; 20 ms is the usual, comfortably conservative value.
// If a real press is missed or double-fires, change only this constant.
static const uint32_t BUTTON_DEBOUNCE_MS = 20;

// Threshold at which a hold becomes a long press. M0 uses short press only
// (page / focus switch). The constant stays so that adding long press at
// M2 does not require restructuring the state machine.
static const uint32_t BUTTON_LONGPRESS_MS = 1000;

// ------------------------------------------------------------------ buzzer
// Active buzzer: sounds at a fixed frequency the moment DC is applied.
// Information can only be encoded in how long it sounds / how long it
// pauses; pitch cannot be changed. Rhythm table: docs/adr/0003.
static const bool BUZZER_ACTIVE_HIGH = true;  // HIGH sounds; flip to false if inverted
static const uint32_t BUZZER_BEEP_MS = 80;    // short beep
static const uint32_t BUZZER_LONG_MS = 400;   // long beep (WiFi lost)
static const uint32_t BUZZER_GAP_MS = 120;    // pause between repeated beeps

// ------------------------------------------------------------------ refresh cadence
// The three screens refresh independently (docs/adr/0001 note), but at two
// different rates:
//   - ESP32 status screen: uptime changes every second -> 1 s redraw
//   - server / mailbox screens: content comes from a 15 s poll, so there
//     is no point redrawing them every second
static const uint32_t REFRESH_LOCAL_MS = 1000;   // ESP32 status screen
static const uint32_t REFRESH_REMOTE_MS = 1000;  // server / mailbox (M0: same)

// How often an absent screen is re-probed, so unplug/replug recovers without
// a reset. Only currently-offline channels are probed, so the steady state
// costs nothing.
static const uint32_t RESCAN_INTERVAL_MS = 5000;

}  // namespace config
