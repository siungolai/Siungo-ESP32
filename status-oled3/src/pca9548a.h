/**
 * pca9548a.h -- PCA9548A 8-channel I2C multiplexer driver
 *
 * The interface follows the design draft in HARDWARE.md section 4.7;
 * this project turns that draft into an implementation.
 *
 * Why "one channel at a time" is a hard rule rather than an optimization:
 *   every downstream branch carries its own pull-up resistors. Selecting
 *   several channels at once ties those pull-ups in parallel onto the
 *   upstream bus, dropping the bus resistance and sharpening the edges.
 *   The ESP32 I2C peripheral does not necessarily survive that, and the
 *   parallel-pull-up risk recorded in HARDWARE.md section 4.4 is exactly
 *   the most common way this chip gets a board killed.
 *   So pca9548a_write_mask(uint8_t mask) is deliberately NOT exposed:
 *   a caller cannot even express "select channels 0 and 1 together",
 *   which rules the misuse out at the interface level.
 *
 * Channel cache: select(n) returns immediately without an I2C transaction
 *   when n is already the current channel. SSD1306 flush writes the same
 *   screen many times per round; rewriting the mux register every time
 *   would add 8 pointless upstream transactions per refresh.
 *
 * Topology note (bench reality, 2026-10-17): when config::ACTIVE_SCREENS is
 *   1 there is no mux in the signal path at all -- the panel is wired
 *   straight to the upstream pins. In that topology begin() returns false
 *   (there is genuinely no mux to bring up) while select() returns TRUE
 *   (the bus already reaches the screen, so "reaching the downstream bus"
 *   succeeds). screen.cpp checks isDirect() to tell the two situations apart;
 *   every other caller should go through screen::, not through this API.
 */
#pragma once

#include <stdint.h>

namespace pca9548a {

// Bring up the I2C bus, set the timeout, probe the mux itself.
// Returns false = no ACK at 0x70 (address jumpers soldered differently /
// not wired / not powered), OR there is no mux in the signal path at all
// (config::ACTIVE_SCREENS <= 1). See isDirect() to tell them apart.
bool begin();

// Whether the mux is online. While it is offline every upstream access
// from the screens short-circuits and no I2C traffic is generated.
// False in the single-screen-direct topology by design.
bool ready();

// True when the firmware is configured with no mux in the signal path
// (config::ACTIVE_SCREENS <= 1): the screen hangs directly off the
// upstream pins, so select()/deselectAll() become bus-level no-ops.
bool isDirect();

// Select exactly this channel and disconnect all others. channel is 0-7;
// out-of-range returns false. Re-selecting the current channel is a no-op.
// In the single-screen-direct topology this is a no-op that returns true.
bool select(uint8_t channel);

// Disconnect all channels. Keeping them disconnected between screen
// refreshes avoids leaving eight branches' pull-ups hanging on the bus.
// In the single-screen-direct topology this is a no-op.
void deselectAll();

// The currently selected channel; 8 means nothing is selected.
uint8_t currentChannel();

// Result code of the most recent upstream I2C transaction (0 = ok, 5 = timeout).
// For serial diagnostics, so callers do not have to re-instrument begin/end.
uint8_t lastError();

}  // namespace pca9548a
