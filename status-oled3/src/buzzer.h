/**
 * buzzer.h -- active buzzer driver, on/off time encoding
 *
 * The buzzer in this project is the ACTIVE type (measured: sounds the moment
 * DC is applied across its two leads). It carries its own oscillator, so the
 * pitch is fixed at whatever the manufacturer built in and cannot be changed
 * by software. Information can only be encoded in HOW LONG it sounds and how
 * long it pauses -- see docs/adr/0003.
 *
 * Consequences for the driver:
 *   - No analogWrite / PWM. PWM on an active buzzer does not change the pitch;
 *     it only chops the sound into a rasp. Any tone an active buzzer seems to
 *     "play" is actually amplitude modulation, and it wastes a LEDC channel.
 *   - No frequency table, no note abstraction. Only "beep for N ms" and
 *     "pause for N ms" exist.
 *
 * The driver is deliberately NOT a background task: sound production is a
 * blocking sequence of on/off steps. That is safe here because the longest
 * pattern (server stale: three short beeps) is under 600 ms and happens once
 * per state change, not every loop. The trade-off is documented in
 * docs/adr/0003: if a pattern ever needs to overlap with a screen refresh,
 * move this to a tick()-driven sequencer like button::tick().
 *
 * M2 note: no mute switch by design (user decision). If a silent mode is
 * ever wanted, add a config flag and check it here, not at the call sites.
 *
 * Drive strength: begin() also raises the buzzer pin's pad drive capability
 * to GPIO_DRIVE_CAP_3 (40 mA source current). The default for GPIO 21 is
 * only 20 mA while a typical active buzzer draws 20-30 mA, so the default
 * is right at the edge. See docs/adr/0007 and the config.h comment.
 */
#pragma once

#include <stdint.h>

namespace buzzer {

// Configure the output pin. Call once from setup(), before beep().
void begin();

// Sound the buzzer for ms milliseconds, then stop. Blocking.
void beep(uint32_t ms);

// Sound the buzzer for ms, pause BUZZER_GAP_MS, repeat times total.
// times < 1 is treated as 1.
void beepRepeated(uint32_t ms, uint8_t times);

// ---- the patterns from docs/adr/0003 ----
// One function per event keeps the rhythm table in exactly one place, so a
// rhythm change is an edit here and nowhere else.
void patternNewMail();      // 1 short beep
void patternServerStale();  // 3 short beeps
void patternWifiLost();     // 1 long beep
void patternButtonEmpty();  // 1 very short beep (button with nothing to do)

// True while a pattern is playing. For serial diagnostics only: nothing in
// the program branches on it.
bool busy();

}  // namespace buzzer
