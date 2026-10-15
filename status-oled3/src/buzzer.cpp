/**
 * buzzer.cpp -- blocking pattern player
 *
 * All patterns are built from beep()/pause only. The durations live in
 * config.h so a rhythm change never needs an edit here.
 */
#include "buzzer.h"

#include <Arduino.h>

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
