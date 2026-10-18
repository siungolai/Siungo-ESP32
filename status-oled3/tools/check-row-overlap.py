"""Regression test: no two strings may be drawn onto the same row of a screen.

Why this exists
---------------
drawText5x7() reaches drawText5x7Y(), which ORs pixels onto the framebuffer:

    buf_[...] |= 1u << (ty % 8);

It never erases ("off pixels do not overwrite what is already there"). So when
two strings land on one row they do not overwrite each other -- their pixels
unite, and the result is a smear that reads as neither. That was the
2026-10-17 bug on the server screen's second page: drawServerPage2() drew
"upl=up" on row 6 and drawServer() then drew "pg=2/2 A=page" on the same row.

The invariant
-------------
Within one screen, one frame, each row is written by at most one string.

What counts as one frame
------------------------
    LOCAL   -> drawLocal()
    SERVER  -> drawServer() + exactly one of drawServerPageN()
    MAILBOX -> drawMailbox()

A draw* function is attributed to a screen by the screen::SCREEN_* constant in
its body, so adding a screen or a page does not need this file edited -- which
matters, because a hardcoded list silently stops covering new code.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "main.cpp"

# Every call that writes one row of text. printKV/kvNumber/kvString funnel
# into screen::printLine, and screen::printLine is in this set too: a caller
# that goes straight to it (rather than through the three wrappers) must be
# seen by this checker, or the one layout bug it exists to catch is silent
# exactly where the code took a new path.
DRAW_CALL = r"(?:printKV|kvNumber|kvString|printLine)"

# The row argument is one of:
#   5                              a literal
#   static_cast<uint8_t>(4 + i)    a probe loop index
#   static_cast<uint8_t>(row)      a variable (not statically checkable)
ROW_ARG = r"(\d+)(\s*\+\s*i)?"


def function_bodies(source):
    """name -> body for every top-level `void name(...)  { ... }` in main.cpp."""
    out = {}
    # Match the function header, then walk to the matching closing brace at
    # column 0 (the file's formatting keeps them there).
    for m in re.finditer(r"^void (\w+)\([^)]*\)\s*\{", source, re.M):
        name = m.group(1)
        depth = 1
        i = m.end()
        while i < len(source) and depth:
            ch = source[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        out[name] = source[m.end():i - 1]
    return out


def screen_of(body):
    """The SCREEN_* constant this draw function WRITES to, or None.

    Only write calls count. drawLocal() also calls screen::online(SCREEN_SERVER)
    to render the "scr=111" line, and treating that read as a target made the
    function look ambiguous -- it returned None and the whole LOCAL screen was
    silently skipped from the check. That is exactly the kind of quiet gap this
    test exists to prevent, so the ambiguity is resolved by matching the calls
    that actually paint.
    """
    ids = set()
    pat = (r"(?:drawTitleBar|" + DRAW_CALL + r")\(\s*screen::(SCREEN_\w+)")
    for m in re.finditer(pat, body):
        ids.add(m.group(1))
    ids.discard("SCREEN_COUNT")
    if len(ids) == 1:
        return ids.pop()
    if ids:
        # More than one: pick the one a title bar is drawn on, since every
        # screen paints exactly one title.
        title = re.search(r"drawTitleBar\(\s*screen::(SCREEN_\w+)", body)
        if title:
            return title.group(1)
    return None


def probe_count(body):
    m = re.search(r"probe_label\[(\d+)\]", body)
    return int(m.group(1)) if m else 3


def scan_draws(body):
    """List (row, label) for every string this body writes.

    The label is the string in the row-writing position, which is the same
    slot in every call shape this checker knows:
      printKV(id, row, "key", value)   -> "key"  (what the operator reads)
      printLine(id, row, "text")       -> "text"
    so one pattern covers both and no per-call special case is needed.
    """
    entries = []
    n = probe_count(body)
    pat = (DRAW_CALL + r"\(\s*(?:screen::)?(?:SCREEN_\w+|\w+)\s*,\s*"
           r"(?:static_cast<uint8_t>\()?\s*" + ROW_ARG + r"\s*\)?\s*,\s*"
           r"(?:\"([^\"]*)\"|(\w+_label)\[i\])")
    for mt in re.finditer(pat, body):
        base = int(mt.group(1))
        is_probe = bool(mt.group(2))
        label = mt.group(3) or mt.group(4) or "?"
        if is_probe:
            for i in range(n):
                entries.append((base + i, "%s[%d]" % (label, i)))
        else:
            entries.append((base, label))
    return entries


def collapse_branches(entries):
    """if/else write the same row with the same key; that is not an overlap.

    Distinct keys on one row, however, is a real collision.
    """
    by_row = {}
    for row, label in entries:
        # probe[0] / probe[1] are distinct rows by construction, so they never
        # collide; key on the label at all for literals only.
        base = label.split("[")[0]
        by_row.setdefault(row, set()).add(base if "[" not in label else label)
    return by_row


def main():
    source = MAIN.read_text(encoding="utf-8")
    bodies = function_bodies(source)

    draw_fns = {n: b for n, b in bodies.items() if n.startswith("draw")}
    by_screen = {}
    for name, body in draw_fns.items():
        sid = screen_of(body)
        if sid is None:
            continue
        by_screen.setdefault(sid, []).append((name, body))

    failures = []
    for sid in sorted(by_screen):
        fns = by_screen[sid]
        # A "frame" for this screen: draw<X>() plus every draw<X>PageN().
        roots = [f for f in fns if not re.search(r"Page\d+$", f[0])]
        pages = [f for f in fns if re.search(r"Page\d+$", f[0])]

        if not roots:
            failures.append("%s: no root draw function found" % sid)
            continue

        # Every row the pages write must be disjoint from the frame rows and
        # from each other's *frame-level* usage; pages are alternatives, so
        # two pages may reuse a row, but a page may not collide with the root.
        root_rows = {}
        for name, body in roots:
            for row, label in scan_draws(body):
                root_rows.setdefault(row, set()).add(label.split("[")[0])
        print("%s root rows: %s"
              % (sid, {r: sorted(v) for r, v in sorted(root_rows.items())}))

        for name, body in pages:
            ent = scan_draws(body)
            print("  %s rows: %s" % (name, sorted({r for r, _ in ent})))
            for row, label in ent:
                if row in root_rows:
                    failures.append(
                        "%s writes row %d (%s) but the frame also writes it "
                        "(%s); drawText5x7 ORs, so the two strings would merge"
                        % (name, row, label, sorted(root_rows[row])))
            for row, labels in sorted(collapse_branches(ent).items()):
                if len(labels) > 1:
                    failures.append(
                        "%s writes row %d with %d different keys: %s"
                        % (name, row, len(labels), sorted(labels)))

        # A root function must not write one row with two different keys.
        for name, body in roots:
            ent = scan_draws(body)
            for row, labels in sorted(collapse_branches(ent).items()):
                if len(labels) > 1:
                    failures.append(
                        "%s writes row %d with %d different keys: %s"
                        % (name, row, len(labels), sorted(labels)))

    if failures:
        print("\nFAIL")
        for f in failures:
            print("  -", f)
        return 1
    print("\nOK: no row of any screen is written by two different strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
else:
    # The filename has a hyphen, so `import check-row-overlap` is not valid
    # Python. The self-test needs the helpers; give it a legal module name.
    sys.modules.setdefault("check_row_overlap_checker", sys.modules[__name__])
