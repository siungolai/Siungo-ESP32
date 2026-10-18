"""Build the pre-fix button.cpp and prove button-selftest.py catches it.

The long-press fix added a `long_fired` latch to button.cpp. A regression test
that never fails is worthless, so this script reconstructs the pre-fix state
machine (the latch removed, `pending` tested directly), runs the same
selftest model against it, and asserts it FAILS.

    python tools/button-selftest-prefix.py

Exit 0 = the selftest really does catch the bug it was written for.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Load the model straight from the selftest so the two can never drift.
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "button_selftest", HERE / "button-selftest.py")
_st = importlib.util.module_from_spec(_spec)
sys.modules["button_selftest"] = _st
_spec.loader.exec_module(_st)

ST_DEBOUNCE_UP = _st.ST_DEBOUNCE_UP


class PreFixButton(_st.Button):
    """button.cpp as it was before the long_fired latch.

    Only the two branches the fix touched differ; everything else is inherited
    so this stays a diff of the fix, not a rewrite of the machine.
    """

    def tick(self, now, raw_pressed):
        st = self.state
        if st == _st.ST_DOWN:
            if not raw_pressed:
                self.state = _st.ST_DEBOUNCE_UP
                self.change_ms = now
            elif now - self.press_ms >= _st.LONGPRESS_MS:
                # No latch: every tick re-enters this branch with press_ms
                # unchanged, so the event fires once per tick.
                self.pending = _st.EVENT_LONG_PRESS
                self.state = _st.ST_DEBOUNCE_UP
                self.change_ms = now
            return
        if st == _st.ST_DEBOUNCE_UP:
            if raw_pressed:
                self.state = _st.ST_DOWN
            elif now - self.change_ms >= _st.DEBOUNCE_MS:
                # Tested against pending, which drain() has already cleared.
                if self.pending != _st.EVENT_LONG_PRESS:
                    self.pending = _st.EVENT_SHORT_PRESS
                self.state = _st.ST_IDLE
            return
        super().tick(now, raw_pressed)


def main():
    failures = []

    # Bug 1: a 3 s hold must NOT spam long presses. Pre-fix it did.
    b = PreFixButton()
    events = _st._hold(b, 0, 3000)
    spam = events.count(_st.EVENT_LONG_PRESS)
    print("pre-fix 3 s hold  :", spam, "long presses")
    if spam == 1:
        failures.append(
            "pre-fix machine emitted only 1 long press; the fixture is wrong "
            "(it no longer reproduces the bug)")

    # Bug 2: drain the long press, then release -- pre-fix added a short press.
    # The drain has to happen while the button is still held, which is where
    # the real firmware drains it (handleButtons() runs every loop() iteration,
    # not on release).
    b = PreFixButton()
    _st._hold(b, 0, 1499)
    b.tick(1500, True)          # this tick is the one that fires the long press
    drained = b.drain()
    if drained != _st.EVENT_LONG_PRESS:
        failures.append("pre-fix drain returned %r, want LONG" % (drained,))
    after = _st._release(b, 1501, 1600)
    print("pre-fix drain+rel :", "drained=%r after=%r" % (drained, after))
    if _st.EVENT_SHORT_PRESS not in after:
        failures.append(
            "pre-fix release after a drained long press emitted %r, want a "
            "spurious SHORT" % (after,))

    print()
    if failures:
        print("SELFTEST FAIL -- the pre-fix fixture no longer reproduces:")
        for f in failures:
            print("  -", f)
        return 1
    print("OK: the selftest catches both bugs the fix removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
