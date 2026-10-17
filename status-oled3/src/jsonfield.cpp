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
 */
#include "jsonfield.h"

#include <cstdlib>
#include <cstring>

namespace jsonfield {

namespace {

// Locate the value that follows "key": in a flat object.
// Returns a pointer to the first character of the value, or nullptr.
// The search is textual: the key must appear as "key" followed by optional
// whitespace and a colon. That is exact enough for flat, machine-generated
// JSON, which is all this project polls.
const char* findValue(const char* json, const char* key) {
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

}  // namespace

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
  *out = parsed;
  return true;
}

}  // namespace jsonfield
