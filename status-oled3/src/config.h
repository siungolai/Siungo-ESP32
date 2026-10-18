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
// Wiring verified on the bench 2026-10-16: the PCA9548A module's SDA/SCL are
// wired to GPIO4 / GPIO5, NOT the GPIO8/9 originally planned in pet-oled3.
// HARDWARE.md section 1.2 marks GPIO4/5 as the measured-working I2C pair.
// GPIO 1/2 are forbidden (HARDWARE.md section 6.1: whole segment times out).
//
// ORDER MATTERS, and got it backwards once. On the bench the wires are
//   GPIO4 -> module SCL,  GPIO5 -> module SDA
// (operator report 2026-10-17: "g4 connects scl; g5 connects sda").
// So GPIO5 drives SDA and GPIO4 drives SCL. Measured by i2c-probe v7: the
// pair (SDA=GPIO5, SCL=GPIO4) ACKs 0x70, the pair (SDA=GPIO4, SCL=GPIO5)
// answers nothing at all -- a swapped pair cannot form a START condition,
// so every transaction returns error 2 with the lines sitting perfectly
// idle HIGH. If 0x70 ever stops answering again, check the order first.
static const uint8_t I2C_SDA_PIN = 5;
static const uint8_t I2C_SCL_PIN = 4;
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

// ------------------------------------------------------------------ screen topology
// One knob: how many physical panels are actually wired.
//
// Bench history, because it is the reason this constant exists at all: the
// PCA9548A module appeared dead at 0x70 for 36 hours because SDA/SCL were
// crossed on the prototype (see the I2C note above). While that was being
// hunted the firmware was switched to single-screen-direct -- one panel wired
// straight to GPIO4/5, no mux in the path -- and that topology is still
// supported. The crossing is fixed, the mux answers again, and the bench is
// back on three screens.
//
// Returning to either topology is this one line, nothing else:
//   ACTIVE_SCREENS = 3 -> the mux is in the path, each logical screen owns a
//                         channel and a panel (docs/adr/0001)   [current]
//   ACTIVE_SCREENS = 1 -> no mux; all logical screens share the one panel
//                         and the buttons choose which one is shown
// The mux is in the signal path exactly when this count is greater than 1,
// so there is only one knob and no combination of settings that is wrong.
//
// Note the two paragraphs that used to sit here contradicted each other (one
// said development ran single-screen-direct, the other that it therefore ran
// single-screen-direct) while the value below said 3. The current state is
// written once, here, and nowhere else claims otherwise.

// ------------------------------------------------------------------ downstream screens
// Role split per docs/adr/0001: ch0 = ESP32 status, ch1 = server, ch2 = mailbox.
// All three screens keep the 0x3C factory address (address conflicts are
// isolated per channel -- HARDWARE.md section 4.6).
static const uint8_t SSD1306_ADDR = 0x3C;

// Logical screens the firmware knows about. This is a software concept and
// stays 3 in every topology: with one panel the three are painted in turn,
// with three panels each owns one.
static const uint8_t SCREEN_COUNT = 3;
static const uint8_t SCREEN_CHANNEL[SCREEN_COUNT] = {0, 1, 2};

// Physical panels actually wired. See the topology note above.
// 1 = single panel straight onto GPIO4/5.
// 3 = mux in the path, one panel per channel (current bench state since the
//     SDA/SCL order was corrected and 0x70 started answering).
static const uint8_t ACTIVE_SCREENS = 3;

// ------------------------------------------------------------------ buttons
// Two tactile buttons, internal pull-up, pressed = LOW.
// Wiring verified on the bench 2026-10-16: button A (page) on GPIO6,
// button B (focus) on GPIO7. GPIO 10/11 were the original plan
// (docs/adr/0007) but the bench wiring used 6/7 instead; ADR-0007 was
// reconciled to the bench values. Buzzer moved off GPIO12 to GPIO21
// because GPIO12 is an MTDI strapping pin.
static const uint8_t PIN_BUTTON_PAGE = 6;    // button A: page on focused screen
static const uint8_t PIN_BUTTON_FOCUS = 7;   // button B: switch focused screen
static const uint8_t PIN_BUZZER = 21;        // active buzzer: on/off encoding

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
//   - the data screens: redrawn when a new document lands, which is the
//     honest cadence for a document the server only rewrites every 15 s
//     (snapshot) or every 5 min (mailbox). A screen that stops receiving
//     data stops being repainted, so what stays on it is visibly the last
//     real document rather than a clock counting up over stale numbers.
static const uint32_t REFRESH_LOCAL_MS = 1000;    // ESP32 status screen
// UNUSED: the data screens are not on a timer at all, they are repainted when
// a document lands (see the comment above). It survived because it was once
// read by a loop that has since been replaced by the dirty-flag dispatch.
static const uint32_t REFRESH_REMOTE_MS = 15000;  // server screen (M1b)

// Rate limit for the "[poll] body parsed but uptime missing" style warnings:
// without it a 200 OK that is the wrong document logs a line every 15 s for
// as long as the misconfiguration lasts.
static const uint32_t POLL_WARN_INTERVAL_MS = 60000;

// How often an absent screen is re-probed, so unplug/replug recovers without
// a reset. Only currently-offline channels are probed, so the steady state
// costs nothing.
static const uint32_t RESCAN_INTERVAL_MS = 5000;

// ------------------------------------------------------------------ network (M1)
// Data sources. M1a polled the main site's public /api/health
// (docs/adr/0009); M1b replaces that one source with the two documents the
// cron collectors write (docs/adr/0008), served by nginx under /status/.
// Both live on the same host and the same TLS path, so TLS, the shared key
// and the link check are shared (src/health.cpp; the M1a file that used to
// hold them, netfetch.cpp, no longer exists) and only the path and the
// cadence differ.
//
// /api/health is NOT polled any more. Keeping both shapes alive would mean
// the parser has to guess which document it is holding, and the server
// screen would have to explain where each number came from. The M1a code is
// preserved in git history (commit 53218a2) if that source is ever needed
// again.
// The three string constants below are constexpr, not `static const char*`.
// The static form is a pointer OBJECT in every translation unit that includes
// this header, so screen.cpp/button.cpp/... each carried three dead pointers
// and a matching -Wunused-variable warning under -Wall. A constexpr pointer is
// a value: nothing to allocate, nothing to warn about.
constexpr const char* DATA_HOST = "www.siungo.top";
constexpr const char* DATA_PATH = "/status/snapshot.json";
constexpr const char* MAILBOX_PATH = "/status/mailbox.json";

// Shared key for M1b, sent as the X-Monitor-Key request header. EMPTY at
// M1a because /api/health is public. The key itself lives in src/secrets.h
// (gitignored), never here; only the header NAME is a build constant.
constexpr const char* MONITOR_KEY_HEADER = "X-Monitor-Key";

// Poll cadence and staleness threshold (docs/adr/0004):
//   poll every 15 s; 60 s without a valid response = "stale".
// Both are multiples of the poll interval so the staleness verdict is
// decided by the device's own monotonic millis() clock, NOT by the
// timestamp inside the JSON (the cron that produces it has minute
// granularity, docs/adr/0008 consequence 3).
static const uint32_t POLL_INTERVAL_MS = 15000;
static const uint32_t STALE_AFTER_MS = 60000;

// Port both documents are served on. Kept as a constant rather than written
// inline in health.cpp because the plain TCP link probe and the TLS fetch
// must agree on it -- if they ever disagree, the probe would prove a path
// that the fetch does not use.
static const uint16_t DATA_PORT = 443;

// The mailbox document is produced by a five-minute cron, so polling it on
// the 15 s cadence would re-fetch an identical body four times out of five.
// It gets its own slower cadence; the server metrics keep the 15 s one.
static const uint32_t MAILBOX_INTERVAL_MS = 300000;

// Staleness is judged per document, not globally: a mailbox cron that has
// not run yet must not make the server screen claim the server is stale.
// The mailbox threshold is therefore ~1.6 cron periods (5 min): a document
// that is one period old is normal, one that is nearly two periods old is
// not being refreshed. The server keeps the 60 s / 4 poll verdict.
static const uint32_t MAILBOX_STALE_AFTER_MS = 480000;

// How often the WiFi link itself is checked (reconnect on drop). Longer than
// the poll interval because a reconnect attempt costs seconds, not
// milliseconds.
static const uint32_t WIFI_CHECK_INTERVAL_MS = 5000;

// WiFi connect timeout inside a single attempt (ms). On timeout the firmware
// keeps running and shows "no data" instead of blocking setup() forever;
// precedent: clock-oled/src/main.cpp connectWiFi().
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// TLS read timeout for one HTTP request (ms). The request itself is small,
// so a slow answer means something is wrong; better to fail the poll and
// retry on the next cadence than to sit and wait (docs/adr/0004: no
// immediate retry).
static const uint32_t HTTP_TIMEOUT_MS = 8000;

// Size of the HTTP response buffer.
//
// 1 KiB is the budget BOTH sides are held to: docs/server/status-snapshot.py
// writes the document (compact separators, no indent) and
// docs/server/status-check.py verifies it still fits this constant -- there is
// a size gate there, plus tools/snapshot-size-probe.py to measure the real
// document. The reason it is worth a gate rather than a guess: a body that
// grows past this cap is truncated by the read loop and then rejected by
// looksComplete(), which on screen is indistinguishable from a dead server,
// and the failure message the device can give ("body truncated at N bytes")
// never mentions the size. The real 19-field document measures 385 bytes, so
// there is room for about 2.5x growth before this bites.
//
// Growing the budget is cheap: these two buffers are 2 KiB of the 320 KB RAM,
// and printStatus() reports the live size every minute.
static const uint16_t HTTP_RESPONSE_MAX = 1024;

// ---- M1b field presentation ------------------------------------------------
// snapshot.json sends raw counters and kB values (docs/adr/0012). Nothing
// on the device divides or reformats them: the point of putting the scaling
// on the server is that the firmware only has to print, so a wrong number
// can always be traced to one place (docs/adr/0012 decision 3).
//
// Two derived numbers do not exist in the JSON and are computed here:
//   mem used percent  = 100 * mem_used_kb / mem_total_kb
//   disk used percent = 100 * disk_used_kb / disk_total_kb
// Both fall back to "-" when either side is missing/null, which is the only
// correct answer: a percentage with an unknown denominator is not a number.
//
// There are no bars on the server screen: the rows carry text, and a row of
// '#' would cost more columns than the value it encodes. The three constants
// below are therefore unused, and are kept only so the reasoning is on record
// -- they were part of an earlier layout idea. Do not add a bar without
// deleting them first, or the file will hold two answers to the same question.
static const uint32_t MEM_BAR_KB = 4;     // one '#' per this many used MB of RAM
static const uint32_t DISK_BAR_GB = 4;    // one '#' per this many used GB of disk
static const uint8_t BAR_SEGMENTS = 21;   // 21 columns of 128 px at 6 px/char

// Serial log line length cap for poll diagnostics.
static const uint16_t LOG_LINE_MAX = 160;

}  // namespace config
