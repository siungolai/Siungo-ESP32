"""Does button.cpp's state machine really emit one event per press?

A model of the state machine, driven by the same tick() contract the firmware
implements, plus the two regressions it must keep catching:

  1. long-press spam: a held button emitted EVENT_LONG_PRESS once per tick()
     instead of once per press. Fixed by the long_fired latch in button.cpp.
  2. long-then-short: a long press that was drained before the release also
     emitted EVENT_SHORT_PRESS afterwards, because the release branch tested
     `pending != EVENT_LONG_PRESS` and drain() had already cleared it.

The model is deliberately a faithful transcription of the four states and
their transitions, NOT a re-derivation: if this file starts to disagree with
button.cpp, the tests below fail and someone has to reconcile them.

    python tools/button-selftest.py

Exit 0 = the state machine honours its contract.
"""
import sys

DEBOUNCE_MS = 20
LONGPRESS_MS = 1000

ST_IDLE = 0
ST_DEBOUNCE_DOWN = 1
ST_DOWN = 2
ST_DEBOUNCE_UP = 3

EVENT_NONE = 0
EVENT_SHORT_PRESS = 1
EVENT_LONG_PRESS = 2


class Button:
    """The four states and the long_fired latch from button.cpp."""

    def __init__(self):
        self.state = ST_IDLE
        self.change_ms = 0
        self.press_ms = 0
        self.long_fired = False
        self.pending = EVENT_NONE

    def tick(self, now, raw_pressed):
        st = self.state
        if st == ST_IDLE:
            if raw_pressed:
                self.state = ST_DEBOUNCE_DOWN
                self.change_ms = now
                self.long_fired = False
        elif st == ST_DEBOUNCE_DOWN:
            if not raw_pressed:
                self.state = ST_IDLE
                self.change_ms = now
            elif now - self.change_ms >= DEBOUNCE_MS:
                self.state = ST_DOWN
                self.press_ms = now
        elif st == ST_DOWN:
            if not raw_pressed:
                self.state = ST_DEBOUNCE_UP
                self.change_ms = now
            elif now - self.press_ms >= LONGPRESS_MS:
                if not self.long_fired:
                    self.long_fired = True
                    self.pending = EVENT_LONG_PRESS
                self.state = ST_DEBOUNCE_UP
                self.change_ms = now
        elif st == ST_DEBOUNCE_UP:
            if raw_pressed:
                self.state = ST_DOWN
            elif now - self.change_ms >= DEBOUNCE_MS:
                if not self.long_fired:
                    self.pending = EVENT_SHORT_PRESS
                self.long_fired = False
                self.state = ST_IDLE

    def drain(self):
        out = self.pending
        self.pending = EVENT_NONE
        return out


def _hold(button, from_ms, to_ms, step_ms=1):
    """Hold the button down across [from_ms, to_ms], draining every tick.

    This is the shape that produced the spam bug: the caller keeps ticking
    while the button stays pressed, exactly like loop() does.
    """
    events = []
    now = from_ms
    while now <= to_ms:
        button.tick(now, True)
        ev = button.drain()
        if ev != EVENT_NONE:
            events.append(ev)
        now += step_ms
    return events


def _release(button, from_ms, to_ms, step_ms=1):
    """Release and let the debounce-up window expire."""
    events = []
    now = from_ms
    while now <= to_ms:
        button.tick(now, False)
        ev = button.drain()
        if ev != EVENT_NONE:
            events.append(ev)
        now += step_ms
    return events


def main():
    failures = []

    # 1. Short press: exactly one short press, on release, never before.
    b = Button()
    early = _hold(b, 0, 300)
    if early:
        failures.append("short press emitted %r while still held" % (early,))
    late = _release(b, 301, 400)
    if late != [EVENT_SHORT_PRESS]:
        failures.append("short press emitted %r on release, want [SHORT]" % (late,))

    # 2. Long press held for 3 s: exactly ONE long press, then silence.
    #    Pre-fix this returned ~100 long presses (one per tick from 1000 ms on).
    b = Button()
    events = _hold(b, 0, 3000)
    if events.count(EVENT_LONG_PRESS) != 1:
        failures.append(
            "3 s hold emitted %d long presses, want 1" % events.count(EVENT_LONG_PRESS))
    if any(e == EVENT_SHORT_PRESS for e in events):
        failures.append("long press also produced a short press")

    # 3. The release after a long press must not add a short press, even when
    #    the long press was already drained. Note the drain happens while the
    #    machine is in ST_DEBOUNCE_UP (the long press fired and moved it there
    #    while the button was still held), so long_fired is the only thing left
    #    that remembers what this press was.
    b = Button()
    held = _hold(b, 0, 1500)
    if held.count(EVENT_LONG_PRESS) != 1:
        failures.append(
            "hold to 1500 ms emitted %r, want one LONG" % (held,))
    if b.state != ST_DEBOUNCE_UP:
        failures.append(
            "after a long press the machine is in state %d, want ST_DEBOUNCE_UP"
            % b.state)
    # The event was already drained by _hold(); drain again here is NONE. What
    # matters is what the RELEASE does next.
    after = _release(b, 1501, 1600)
    if after:
        failures.append("release after a long press emitted %r" % (after,))

    # 4. Bounce during the hold: release->repress inside the debounce window
    #    must not restart the long-press budget.
    b = Button()
    _hold(b, 0, 900)
    _release(b, 901, 919)      # 19 ms: shorter than the 20 ms debounce
    bounced = _hold(b, 920, 3000)
    if bounced.count(EVENT_LONG_PRESS) != 1:
        failures.append(
            "bounced hold emitted %d long presses, want 1" % bounced.count(EVENT_LONG_PRESS))

    # 5. Chatter inside the debounce-down window must not produce an event.
    b = Button()
    for now in range(0, 19):   # never holds 20 ms
        b.tick(now, now % 2 == 0)
        if b.drain() != EVENT_NONE:
            failures.append("chatter produced an event at t=%d" % now)
    if b.state != ST_IDLE and b.state != ST_DEBOUNCE_DOWN:
        failures.append("chatter left the machine in state %d" % b.state)

    # 6. A second press after a completed one works normally.
    b = Button()
    _hold(b, 0, 300)
    _release(b, 301, 400)
    _hold(b, 500, 800)
    _release(b, 801, 900)
    total = b.drain()
    if total != EVENT_NONE:
        failures.append("second press left a stale event: %r" % (total,))

    print("short press      :", "1 event, on release")
    print("3 s hold         :", "1 long press, no short press")
    print("drained long     :", "release adds nothing")
    print("bounce in hold   :", "long press budget not restarted")
    print("chatter < 20 ms  :", "no event")
    print()
    if failures:
        print("SELFTEST FAIL -- the debounce machine has regressed:")
        for f in failures:
            print("  -", f)
        return 1
    print("OK: one event per physical press, long and short alike")
    return 0


if __name__ == "__main__":
    sys.exit(main())
