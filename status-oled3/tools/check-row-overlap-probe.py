"""Probe: does check-row-overlap.py see a screen::printLine write?

    python tools/check-row-overlap-probe.py

Builds a throwaway copy of the project tree with an artificial printLine()
collision injected into drawLocal(), runs the real checker against it, and
asserts the collision is reported.

Why this exists: DRAW_CALL was widened to include screen::printLine, and
"it did not crash" is not evidence that the widening works. This is the guard
on the checker itself -- the checker is the only thing that catches the row-6
class of bug (21e6e53), so a blind spot in it is a blind spot in the project.

The checker resolves src/main.cpp relative to its own file, so the probe must
copy the checker, not just point it at a directory.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "main.cpp"
CHECKER = ROOT / "tools" / "check-row-overlap.py"
PYTHON = sys.executable


def main():
    source = MAIN.read_text(encoding="utf-8")
    # printKV writes row 5 with key "ip"; a printLine on the same row with
    # different text is the same bug class as the one 21e6e53 fixed.
    marker = "void drawLocal() {"
    assert marker in source, "drawLocal not found; main.cpp moved"
    injected = (
        "\n  // probe-only: this must be seen by the checker\n"
        "  screen::printLine(screen::SCREEN_LOCAL, 5, \"probe-collide\");\n"
    )
    patched = source.replace(marker, marker + injected, 1)

    tmp = Path(tempfile.mkdtemp())
    try:
        (tmp / "src").mkdir()
        (tmp / "tools").mkdir()
        (tmp / "src" / "main.cpp").write_text(patched, encoding="utf-8")
        shutil.copy(str(CHECKER), str(tmp / "tools" / "check-row-overlap.py"))
        proc = subprocess.run(
            [PYTHON, str(tmp / "tools" / "check-row-overlap.py")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = proc.stdout.decode("utf-8", "replace")
        caught = "probe-collide" in out
        print(out)
        print("checker sees a screen::printLine collision: %s" % caught)
        if not caught:
            print("FAIL: printLine writes are invisible to the checker")
            return 1
        print("OK: printLine writes are checked")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
