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
 * pattern (alert: three short beeps) is under 600 ms and happens once
 * per state change, not every loop. The trade-off is documented in
 * docs/adr/0003: if a pattern ever needs to overlap with a screen refresh,
 * move this to a tick()-driven sequencer like button::tick().
 *
 * M2 note: no mute switch by design (user decision). If a silent mode is
 * ever wanted, add a config flag and check it here, not at the call sites.
 */
#pragma once

#include <stdint.h>

namespace buzzer {

// Configure the output pin. Call once from setup(), before beep().
//
// This also raises the pin's drive strength to GPIO_DRIVE_CAP_3, which
// pinMode() alone does not do. See the comment in buzzer.cpp and
// docs/adr/0010-buzzer-direct-drive-needs-strongest-drive-cap.md.
void begin();

// Sound the buzzer for ms milliseconds, then stop. Blocking.
void beep(uint32_t ms);

// Sound the buzzer for ms, pause BUZZER_GAP_MS, repeat times total.
// times < 1 is treated as 1.
void beepRepeated(uint32_t ms, uint8_t times);

// ---- the patterns of this project (docs/策划案.md section 4.4) ----
// One function per event keeps the rhythm table in exactly one place, so a
// rhythm change is an edit here and nowhere else.
//
// The four original status-display sounds (click / alert / ascending
// "level up" melody / low refusal tone) CANNOT be reproduced on an active
// buzzer: the pitch is fixed in hardware, so only duration and count can
// carry information. "Level up" is therefore ONE LONG beep, not an
// ascending run. See docs/adr/0006.
void patternKeyClick();    // 1 very short click: a key press was accepted
void patternActionDone();  // 1 short beep: an action took effect
void patternAlert();       // 3 short beeps: a number crossed the threshold
void patternLevelUp();     // 1 long beep: growth stage advanced
void patternUnhappy();     // 2 short beeps: the pet is not happy with that

// True while a pattern is playing. For serial diagnostics only: nothing in
// the program branches on it.
bool busy();

}  // namespace buzzer
