/**
 * jsonfield.h -- minimal "get one value by key" JSON reader
 *
 * Scope, stated up front (docs/adr/0010): this is NOT a JSON library. It
 * reads a FLAT object of string and number values and nothing else. No
 * nested objects, no arrays, no traversal, no serialization. Both data
 * sources in this project are flat by design:
 *   - https://www.siungo.top/api/health      (docs/adr/0009)
 *   - https://www.siungo.top/status/snapshot.json (docs/adr/0008)
 * If a future endpoint needs nesting, flatten the SERVER output first --
 * keeping a parser in firmware is the more expensive side (ADR-0010 notes).
 *
 * Neither function allocates: the caller supplies the output buffer, which
 * matches the rest of this project (static framebuffers, no heap).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jsonfield {

// Returns the value of "key" as a NUL-terminated string, or "" when the key
// is absent, the value is not a string, or the JSON is malformed.
// The caller must treat "" as "this field is unavailable", never as "0" or
// "ok". out must have room for cap bytes including the terminator.
const char* field(const char* json, const char* key, char* out, size_t cap);

// Returns true and writes the number when the value of "key" is a JSON
// number. Returns false when the key is absent or the value is not numeric.
// Numbers and strings are separate paths on purpose (ADR-0010 edge case 2):
// a "37.5" string would otherwise be silently parsed with atof().
bool number(const char* json, const char* key, double* out);

}  // namespace jsonfield
