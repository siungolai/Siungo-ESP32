/**
 * pca9548a.cpp -- PCA9548A driver implementation
 *
 * The control register is a single byte where bit n = 1 selects channel n.
 * The write must be one complete transaction (beginTransmission -> write ->
 * endTransmission); the channel only takes effect after the STOP
 * (HARDWARE.md section 4.2, item 2).
 */
#include "pca9548a.h"

#include <Wire.h>

#include "config.h"

namespace pca9548a {

namespace {

// Sentinel for "no channel selected". 8 rather than -1: channel numbers are
// 0-7, so 8 is inside uint8_t range yet impossible to confuse with a channel.
constexpr uint8_t kNoChannel = 8;

bool g_ready = false;
uint8_t g_current = kNoChannel;
uint8_t g_lastError = 0;

}  // namespace

bool begin() {
  // The upstream bus is fixed at begin() time; every downstream access
  // afterwards rides this same bus.
  Wire.begin(config::I2C_SDA_PIN, config::I2C_SCL_PIN, config::I2C_HZ);
  Wire.setTimeOut(config::I2C_TIMEOUT_MS);

  Wire.beginTransmission(config::PCA9548A_ADDR);
  g_lastError = Wire.endTransmission();
  g_ready = (g_lastError == 0);
  g_current = kNoChannel;  // register contents after reset are unknown
  return g_ready;
}

bool ready() { return g_ready; }

uint8_t currentChannel() { return g_current; }

uint8_t lastError() { return g_lastError; }

bool select(uint8_t channel) {
  if (channel > 7) {
    return false;  // out of range: better to fail than to write a byte
                   // that selects the wrong channel
  }
  if (!g_ready) {
    return false;  // mux offline; a write would not reach anywhere anyway
  }
  if (g_current == channel) {
    return true;  // already selected: no-op, saves one upstream transaction
  }

  // Set only the target channel's bit and clear the rest -- this is where
  // "one channel at a time" is enforced. No mask variant is offered, so a
  // caller cannot select several channels at once even by accident.
  Wire.beginTransmission(config::PCA9548A_ADDR);
  Wire.write(static_cast<uint8_t>(1u << channel));
  g_lastError = Wire.endTransmission();
  if (g_lastError != 0) {
    return false;  // write failed: leave the cache alone, retry next time
  }

  g_current = channel;
  return true;
}

void deselectAll() {
  if (!g_ready) {
    return;
  }
  if (g_current == kNoChannel) {
    return;  // already disconnected: no-op
  }

  Wire.beginTransmission(config::PCA9548A_ADDR);
  Wire.write(0x00);
  g_lastError = Wire.endTransmission();
  if (g_lastError == 0) {
    g_current = kNoChannel;
  }
}

}  // namespace pca9548a
