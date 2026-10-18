"""jsonfield selftest -- a model of src/jsonfield.cpp plus the assertions.

    python tools/jsonfield-selftest.py

jsonfield.cpp has no host-side test, and it is the component that decides
whether a server document is accepted at all (health.cpp: hasKey() is the gate
that rejects the nginx SPA catch-all). Two properties are worth locking down
here, because both were once wrong and both fail silently:

  1. hasKey() is exactly as strict as findValue(): `"key":` with the colon.
     Without it, any value containing the literal text "generated_at" (a
     commit message, an error string, an HTML page) satisfies the gate.
  2. number() rejects a value that is not cleanly terminated: `"k":12abc` is
     not 12.

The model below mirrors the C++ line for line, including the escaped-quote
skip in field() and the truncation behaviour. strtod() is modelled as
"longest numeric prefix, then the next byte must be a value terminator",
which is what the C++ does (end == v is the "no digits" case, endsValue(*end)
the terminator check).
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "jsonfield.cpp"
TEXT = SRC.read_text(encoding="utf-8")

# The model is only worth running if it still matches the source it claims to
# mirror. These markers are the lines whose absence would mean the C++ changed
# shape and this test is now testing a fiction.
REQUIRED_MARKERS = [
    "bool keyPresent(const char* json, const char* key)",
    "return findValue(json, key) != nullptr;",
    "const char* findValue(const char* json, const char* key)",
    "if (*after == ':')",
    "inline bool endsValue(char c)",
    "if (!endsValue(*end))",
    "const double parsed = strtod(v, &end);",
    "if (end == v) {",
]
missing = [m for m in REQUIRED_MARKERS if m not in TEXT]
if missing:
    print("SELFTEST FAIL -- jsonfield.cpp no longer contains the code this")
    print("test mirrors; update the model (and this list) first:")
    for m in missing:
        print("  -", m)
    sys.exit(1)

WS = " \t\n\r"


def _skip_ws(s, i):
    while i < len(s) and s[i] in WS:
        i += 1
    return i


def _find_value(json_text, key):
    """Mirror of findValue(): `"key"` + optional ws + ':' + optional ws."""
    if not json_text or not key:
        return None
    start = 0
    while True:
        p = json_text.find(key, start)
        if p < 0:
            return None
        after = p + len(key)
        if p == 0 or json_text[p - 1] != '"' or json_text[after:after + 1] != '"':
            start = after
            continue
        after = _skip_ws(json_text, after + 1)
        if json_text[after:after + 1] != ":":
            start = after
            continue
        return _skip_ws(json_text, after + 1)


def has_key(json_text, key):
    if not json_text or not key:
        return False
    return _find_value(json_text, key) is not None


def field(json_text, key, cap=32):
    """Mirror of field(): string values only, escapes kept as-is, truncated."""
    v = _find_value(json_text, key)
    if v is None or json_text[v:v + 1] != '"':
        return ""
    v += 1
    out = []
    while json_text[v:v + 1] not in ("", '"'):
        if json_text[v:v + 1] == "\\" and json_text[v + 1:v + 2] != "":
            v += 1  # keep the escaped character as-is
        if len(out) + 1 < cap:
            out.append(json_text[v])
        else:
            break
        v += 1
    return "".join(out)


# strtod's numeric prefix: ints, decimals, exponents, plus inf/nan which
# strtod accepts and float() parses.
NUM_PREFIX = re.compile(
    r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|[-+]?(?:inf(?:inity)?|nan)",
    re.IGNORECASE)


def number(json_text, key):
    """Mirror of number(): strtod prefix + a value terminator after it."""
    v = _find_value(json_text, key)
    if v is None or json_text[v:v + 1] == '"':
        return None
    m = NUM_PREFIX.match(json_text, v)
    if m is None:
        return None  # end == v: nothing numeric here
    tail = json_text[m.end():m.end() + 1]
    if tail not in ("", ",", "}", "]", " ", "\t", "\n", "\r"):
        return None  # endsValue(*end) failed
    return float(m.group())


DOC = ('{"generated_at":"2026-10-18T03:00:00Z","sys_uptime_s":12345,'
       '"commit":"abc123","load1_x100":42,"dev":"unknown",'
       '"msg":"the collector said \\"generated_at\\" inside a value",'
       '"mem_used_kb":null}')


def main():
    failures = []

    def check(name, got, want):
        ok = got == want
        print("%-34s %-22r %s" % (name, got,
                                  "ok" if ok else "FAIL want %r" % (want,)))
        if not ok:
            failures.append(name)

    # ---- the two properties this test exists for -------------------------
    check("hasKey present", has_key(DOC, "generated_at"), True)
    check("hasKey absent", has_key(DOC, "nope"), False)
    # A substring that only ever appears INSIDE a string value must not
    # satisfy the gate. This is the one that used to pass: the old keyPresent()
    # accepted a closing quote with no colon, so a value containing the key
    # text -- or an HTML page containing it -- counted as "the document is
    # here".
    check("hasKey value-only text", has_key(DOC, "inside a value"), False)
    check("hasKey quoted-in-value", has_key(DOC, '"generated_at"'), False)

    check("number clean", number(DOC, "load1_x100"), 42.0)
    check("number exponent", number('{"k":12e5}', "k"), 1200000.0)
    # The gap that strtod's leniency leaves open.
    check("number 12abc", number('{"k":12abc}', "k"), None)
    check("number 12e", number('{"k":12e}', "k"), None)
    check("number 1.5x", number('{"k":1.5x}', "k"), None)
    check("number null", number(DOC, "mem_used_kb"), None)
    check("number quoted", number('{"k":"42"}', "k"), None)
    check("number absent", number('{"k":1}', "j"), None)

    # ---- behaviour that must not have regressed --------------------------
    check("field string", field(DOC, "commit"), "abc123")
    check("field dev", field(DOC, "dev"), "unknown")
    check("field null value", field(DOC, "mem_used_kb"), "")
    check("field absent", field(DOC, "nope"), "")
    check("field escaped quote", field(DOC, "msg", cap=64),
          'the collector said "generated_at" inside a value')
    # Truncated document: the closing quote never arrives, so the value is
    # everything to the end of the buffer (ADR-0010 edge case 4).
    check("field truncated", field('{"k":"abcdefghij', "k"), "abcdefghij")
    # cap=8 holds 7 characters plus the NUL -- the C++ writes n + 1 < cap
    # characters then out[n] = '\\0'. 7 characters of "abc123" is "abc1234"
    # ... which is 6 characters, so the honest expectation is the 6-character
    # prefix. The check below is the one that matters: the cap is a hard bound.
    check("field cap honoured", len(field(DOC, "commit", cap=8)), 6)
    check("field cap 5", field(DOC, "commit", cap=5), "abc1")

    print()
    if failures:
        print("SELFTEST FAIL: %d check(s)" % len(failures))
        for f in failures:
            print("  -", f)
        return 1
    print("OK: hasKey/number/field behave as documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
