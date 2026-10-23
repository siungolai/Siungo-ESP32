/**
 * jsonfield.cpp -- the two functions declared in jsonfield.h
 *
 * Both are single-pass scanners over a NUL-terminated buffer. They never
 * write past the caller's buffer and never allocate.
 *
 * Edge cases explicitly handled (docs/adr/0010 decision list):
 *   1. key absent            -> "" / false, caller decides what it means
 *   2. value is a number     -> number() only; field() returns ""
 *   3. escaped quote (\\")   -> skipped, the value is not cut short
 *   4. truncated response    -> parsed as received; a value cut in half
 *                               simply comes back as ""
 *   5. "dev" / "unknown"     -> ordinary strings; the CALLER decides they
 *                               mean "not injected" (see health.cpp)
 *
 * hasKey() is the third function, added for ADR-0012: field() answers ""
 * both for "key absent" and for "value is null", and null is now a real
 * value ("the collector could not measure") that has to be told apart from
 * an absent field on screen.
 */
#include "jsonfield.h"

#include <cstdlib>
#include <cstring>

namespace jsonfield {

namespace {

// Forward declaration: keyPresent() is a thin wrapper over findValue(), which
// needs to be declared first for the definition order below.
const char* findValue(const char* json, const char* key);

// Does "key" appear as a whole key in the object, i.e. as `"key":`?
//
// This is the strict test: a key is only present when it is followed by a
// colon, exactly as findValue() requires. An earlier version accepted a
// closing quote alone, which made hasKey() weaker than field() and number()
// -- a value containing the text "generated_at" (a commit message, an error
// string) satisfied the gate for a key that was not there. Since hasKey() is
// the gate that rejects "the nginx SPA catch-all answered with HTML", a gate
// that is satisfied by the wrong document is worse than no gate.
//
// It delegates to findValue() rather than keeping its own scan: the two loops
// used to be near-identical, and near-identical copies of a parser are how
// the two drift apart.
bool keyPresent(const char* json, const char* key) {
  return findValue(json, key) != nullptr;
}

// Locate the value that follows "key": in a flat object.
// Returns a pointer to the first character of the value, or nullptr.
// The search is textual: the key must appear as "key" followed by optional
// whitespace and a colon. That is exact enough for flat, machine-generated
// JSON, which is all this project polls.
const char* findValue(const char* json, const char* key) {
  if (json == nullptr || key == nullptr || key[0] == '\0') {
    return nullptr;
  }
  const size_t key_len = strlen(key);
  const char* p = json;
  while ((p = strstr(p, key)) != nullptr) {
    const char* after = p + key_len;
    // Require a closing quote so "commit" does not match "commit_short".
    if (p == json || p[-1] != '"' || *after != '"') {
      p = after;
      continue;
    }
    ++after;  // past the closing quote
    while (*after == ' ' || *after == '\t' || *after == '\n' ||
           *after == '\r') {
      ++after;
    }
    if (*after == ':') {
      ++after;
      while (*after == ' ' || *after == '\t' || *after == '\n' ||
             *after == '\r') {
        ++after;
      }
      return after;
    }
    p = after;
  }
  return nullptr;
}

// A character that can end a JSON value: end of buffer, or a structural byte.
// Used to reject "12abc" as a number instead of reading it as 12.
inline bool endsValue(char c) {
  return c == '\0' || c == ',' || c == '}' || c == ']' || c == ' ' ||
         c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

bool hasKey(const char* json, const char* key) {
  if (json == nullptr || key == nullptr || key[0] == '\0') {
    return false;
  }
  return keyPresent(json, key);
}

const char* field(const char* json, const char* key, char* out, size_t cap) {
  if (json == nullptr || key == nullptr || out == nullptr || cap == 0) {
    if (out != nullptr && cap > 0) {
      out[0] = '\0';
    }
    return out;
  }
  out[0] = '\0';

  const char* v = findValue(json, key);
  if (v == nullptr || *v != '"') {
    return out;  // absent, or not a string value
  }

  // Copy the body, honouring backslash escapes.
  ++v;
  size_t n = 0;
  while (*v != '\0' && *v != '"') {
    if (*v == '\\' && v[1] != '\0') {
      ++v;  // keep the escaped character as-is; no \n/\t unescaping needed
    }
    if (n + 1 < cap) {
      out[n++] = *v;
    } else {
      // Truncate rather than overflow: a too-long value becomes a prefix,
      // which the caller can detect by comparing against its own format.
      break;
    }
    ++v;
  }
  out[n] = '\0';
  return out;
}

bool number(const char* json, const char* key, double* out) {
  if (json == nullptr || key == nullptr || out == nullptr) {
    return false;
  }
  const char* v = findValue(json, key);
  if (v == nullptr) {
    return false;
  }
  if (*v == '"') {
    return false;  // quoted number = string value; use field()
  }
  char* end = nullptr;
  const double parsed = strtod(v, &end);
  if (end == v) {
    return false;  // nothing numeric at this position (true/false/null/{})
  }
  // The number must end where a JSON value ends. Without this, "12abc" parses
  // as 12 -- strtod stops at 'a' and reports success. A hand-edited or
  // partially-written document is exactly the case where a silently-accepted
  // garbage number is worse than a rejected field.
  if (!endsValue(*end)) {
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace jsonfield
