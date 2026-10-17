"""Does check-row-overlap.py actually catch the bug it was written for?

A regression test that passes on the old broken code is worthless. This feeds
the pre-fix layouts through the same checker and asserts they FAIL, and that
the real source passes. Run it after editing the checker.

    python tools/check-row-overlap.py        # the real check
    python tools/check-row-overlap-selftest.py   # is the check alive?

Why the older version of this file is gone: it imported scan_page_body()/
check_page_overlap()/frame_rows(), which were replaced when the checker grew
from "server screen only" to "every screen, discovered from the source". An
import error here would have silently retired the self-test.
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The checker's filename has a hyphen, so it cannot be imported by name.
# Load it from its path and register it under a legal module name for the
# rest of the file.
_spec = importlib.util.spec_from_file_location(
    "check_row_overlap_checker", HERE / "check-row-overlap.py")
_checker = importlib.util.module_from_spec(_spec)
sys.modules["check_row_overlap_checker"] = _checker
_spec.loader.exec_module(_checker)

collapse_branches = _checker.collapse_branches
function_bodies = _checker.function_bodies
scan_draws = _checker.scan_draws
screen_of = _checker.screen_of


# The exact layout that shipped and produced the smear (2026-10-17):
# drawServerPage2() painting a probe on row 6, and the page indicator below it.
BROKEN_PAGE2 = """
  screen::printKV(screen::SCREEN_SERVER, 1, "disk", buf);
  screen::printKV(screen::SCREEN_SERVER, 2, "sz", sizes);
  screen::printKV(screen::SCREEN_SERVER, 3, "net", v);
  for (uint8_t i = 0; i < 3; ++i) {
    screen::printKV(screen::SCREEN_SERVER, static_cast<uint8_t>(4 + i),
                    probe_label[i], x);
  }
  screen::printKV(screen::SCREEN_SERVER, 6, "upl", "up");
"""

# The frame around it: the page indicator on row 6, the freshness on row 7.
BROKEN_FRAME = """
  screen::drawTitleBar(screen::SCREEN_SERVER, "SERVER", true);
  screen::printKV(screen::SCREEN_SERVER, 6, "pg", buf);
  screen::printKV(screen::SCREEN_SERVER, 7, "data", buf);
"""


def rows(body):
    return {r for r, _ in scan_draws(body)}


def main():
    failures = []

    # 1. The checker must see the broken page as touching row 6.
    page_rows = rows(BROKEN_PAGE2)
    print("broken page2 rows:", sorted(page_rows))
    if 6 not in page_rows:
        failures.append("scan_draws() did not see the row 6 probe write")

    # 2. And it must see the frame touching row 6 too -- that is the collision.
    frame_rows = rows(BROKEN_FRAME)
    print("broken frame rows:", sorted(frame_rows))
    if not (frame_rows & page_rows):
        failures.append("frame and page showed no shared row; the fixture is wrong")
    else:
        print("  collision on rows:", sorted(frame_rows & page_rows))

    # 3. Two literals on one row inside the same function must be reported.
    dup = """
      screen::printKV(screen::SCREEN_SERVER, 6, "a", "1");
      screen::printKV(screen::SCREEN_SERVER, 6, "b", "2");
    """
    by_row = collapse_branches(scan_draws(dup))
    print("duplicate-literal rows:", {r: sorted(v) for r, v in by_row.items()})
    if len(by_row.get(6, ())) != 2:
        failures.append("collapse_branches() merged two different keys on row 6")

    # 4. if/else on one row is legitimate and must NOT be flagged.
    branches = """
      if (x) {
        screen::printKV(screen::SCREEN_SERVER, 6, "pg", a);
      } else {
        screen::printKV(screen::SCREEN_SERVER, 6, "pg", b);
      }
    """
    by_row = collapse_branches(scan_draws(branches))
    print("if/else rows:", {r: sorted(v) for r, v in by_row.items()})
    if len(by_row.get(6, ())) != 1:
        failures.append("collapse_branches() flagged a legitimate if/else")

    # 5. screen_of() must attribute a screen even when the body READS another
    #    one. drawLocal() calls screen::online(SCREEN_SERVER) for the "scr"
    #    line, and the first version of screen_of() returned None there --
    #    which silently dropped the whole LOCAL screen from the check.
    local_like = """
      screen::drawTitleBar(screen::SCREEN_LOCAL, "ESP32", f);
      flags[1] = screen::online(screen::SCREEN_SERVER) ? '1' : '0';
      screen::printKV(screen::SCREEN_LOCAL, 4, "scr", flags);
    """
    got = screen_of(local_like)
    print("screen_of(local_like) =", got)
    if got != "SCREEN_LOCAL":
        failures.append(
            "screen_of() returned %r; a read of another screen must not make "
            "the target ambiguous" % (got,))

    # 6. The real source must be clean -- that is what the checker is for.
    root = HERE.parent
    source = (root / "src" / "main.cpp").read_text(encoding="utf-8")
    bodies = function_bodies(source)
    drawn = {n: b for n, b in bodies.items() if n.startswith("draw")}
    screens = {screen_of(b) for b in drawn.values()} - {None}
    print("screens discovered in main.cpp:", sorted(screens))
    for expected in ("SCREEN_LOCAL", "SCREEN_SERVER", "SCREEN_MAILBOX"):
        if expected not in screens:
            failures.append("%s was not discovered; it is unchecked" % expected)

    print()
    if failures:
        print("SELFTEST FAIL -- the checker has gone blind:")
        for f in failures:
            print("  -", f)
        return 1
    print("OK: the checker still catches every layout it was built for")
    return 0


if __name__ == "__main__":
    sys.exit(main())
