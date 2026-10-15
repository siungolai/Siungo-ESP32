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
 */
#pragma once

#include <stdint.h>

namespace pca9548a {

// Bring up the I2C bus, set the timeout, probe the mux itself.
// Returns false = no ACK at 0x70 (address jumpers soldered differently /
// not wired / not powered).
bool begin();

// Whether the mux is online. While it is offline every upstream access
// from the screens short-circuits and no I2C traffic is generated.
bool ready();

// Select exactly this channel and disconnect all others. channel is 0-7;
// out-of-range returns false. Re-selecting the current channel is a no-op.
bool select(uint8_t channel);

// Disconnect all channels. Keeping them disconnected between screen
// refreshes avoids leaving eight branches' pull-ups hanging on the bus.
void deselectAll();

// The currently selected channel; 8 means nothing is selected.
uint8_t currentChannel();

// Result code of the most recent upstream I2C transaction (0 = ok, 5 = timeout).
// For serial diagnostics, so callers do not have to re-instrument begin/end.
uint8_t lastError();

}  // namespace pca9548a
