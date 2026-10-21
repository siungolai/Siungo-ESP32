/**
 * buzzer.cpp -- blocking pattern player
 *
 * All patterns are built from beep()/pause only. The durations live in
 * config.h so a rhythm change never needs an edit here.
 *
 * begin() raises the drive strength of the buzzer pin to
 * GPIO_DRIVE_CAP_3. See the comment on begin() below and
 * pet-oled3/docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md
 * (the same buzzer on the same pin, measured in the other project).
 */
#include "buzzer.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "config.h"

namespace buzzer {

namespace {

// The idle (silent) level of the pin. BUZZER_ACTIVE_HIGH is true for the
// buzzer measured in this project (sounds when driven HIGH). Flip the flag
// in config.h instead of editing this file if a module turns out to be
// active-low.
constexpr int kSilentLevel = config::BUZZER_ACTIVE_HIGH ? LOW : HIGH;
constexpr int kSoundLevel = config::BUZZER_ACTIVE_HIGH ? HIGH : LOW;

bool g_busy = false;

}  // namespace

void begin() {
  // Drive strength FIRST, before the pin is even an output.
  //
  // The buzzer is wired GPIO21 -> buzzer+ , buzzer- -> GND, so the ESP32
  // SOURCES the current. What limits it is the pad's drive capability:
  //
  //   ESP32-S3 datasheet v2.2 section 2.3.4 note 5 gives the DEFAULT drive
  //   strength per pin group: GPIO17/18 = 10 mA, GPIO19/20 = 40 mA, all
  //   other pins = 20 mA. GPIO21 is in the "all other" group, so its
  //   default is 20 mA.
  //
  //   The IO electrical characteristics table gives I_OH = 40 mA (VDD 3.3 V,
  //   V_OH >= 2.64 V) when PAD_DRIVER = 3, and I_OL = 28 mA.
  //
  // A typical active buzzer draws 20-30 mA, so the default 20 mA cap is
  // right at the edge: the buzzer either sounds thin or, on a cold morning,
  // does not sound at all.
  //
  // Arduino's pinMode() does NOT do this. In this core __pinMode() only
  // calls gpio_config(); it never touches the pad driver register. The
  // default is GPIO_DRIVE_CAP_2 (medium). So the strength has to be set
  // explicitly, with gpio_set_drive_capability() from the IDF driver header.
  //
  // The header is already in the include chain (Arduino.h -> esp32-hal.h ->
  // esp32-hal-dac.h -> driver/gpio.h), but it is included explicitly here
  // anyway: relying on a transitive include is a trap that breaks silently
  // when someone reorganises a header.
  const esp_err_t err = gpio_set_drive_capability(
      static_cast<gpio_num_t>(config::PIN_BUZZER),
      static_cast<gpio_drive_cap_t>(config::BUZZER_DRIVE_CAP));
  if (err != ESP_OK) {
    // Nothing sensible to do at runtime, but the bench must be able to see
    // that it happened: a silently weak buzzer is exactly the kind of fault
    // that gets blamed on the hardware.
    Serial.print(F("[buzzer] gpio_set_drive_capability failed: "));
    Serial.println(static_cast<int>(err));
  }

  pinMode(config::PIN_BUZZER, OUTPUT);
  digitalWrite(config::PIN_BUZZER, kSilentLevel);
  g_busy = false;
}

void beep(uint32_t ms) {
  if (ms == 0) {
    return;
  }
  g_busy = true;
  digitalWrite(config::PIN_BUZZER, kSoundLevel);
  delay(ms);
  digitalWrite(config::PIN_BUZZER, kSilentLevel);
  g_busy = false;
}

void beepRepeated(uint32_t ms, uint8_t times) {
  if (times < 1) {
    times = 1;
  }
  for (uint8_t i = 0; i < times; ++i) {
    if (i > 0) {
      delay(config::BUZZER_GAP_MS);  // pause between beeps, part of the encoding
    }
    beep(ms);
  }
}

void patternNewMail() {
  // One short beep: the most frequent event, so it must be the shortest
  // pattern that is still audible from arm's length.
  beep(config::BUZZER_BEEP_MS);
}

void patternServerStale() {
  // Three short beeps: server data has not updated for STALE_AFTER_MS.
  // Distinguished from new-mail by count, since an active buzzer cannot
  // vary pitch.
  beepRepeated(config::BUZZER_BEEP_MS, 3);
}

void patternWifiLost() {
  // One long beep: WiFi dropped. Length distinguishes it from every other
  // pattern, so it can be recognized without counting.
  beep(config::BUZZER_LONG_MS);
}

void patternButtonEmpty() {
  // Very short click: the button did nothing (e.g. paging a screen that has
  // only one page). Optional feedback, cheap to keep.
  beep(config::BUZZER_BEEP_MS / 4);
}

bool busy() {
  return g_busy;
}

}  // namespace buzzer
