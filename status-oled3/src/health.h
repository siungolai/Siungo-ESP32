/**
 * health.h -- M1 data path: WiFi link, HTTPS poll, staleness verdict
 *
 * Owns everything about "is the server's data fresh" so that main.cpp only
 * has to read three values (netOnline / stale / secondsSinceData).
 *
 * Cadence (docs/adr/0004): poll every POLL_INTERVAL_MS (15 s). A response
 * counts as valid data only when ALL of these hold:
 *   1. HTTP status is 200,
 *   2. the body parses as JSON with the expected fields,
 *   3. the body is not an error page.
 * There is no immediate retry after a failure -- the next poll is 15 s away
 * by design, so a dead server is not hammered.
 *
 * Staleness (docs/adr/0004, docs/adr/0008 consequence 3): measured on the
 * device's own monotonic millis() clock, never on the timestamp inside the
 * JSON. The cron that produces snapshot.json has minute granularity, so a
 * JSON timestamp would make "the file is 40 s old" look normal.
 *
 * M1a endpoint (docs/adr/0009): the main site's public GET /api/health.
 * It carries no CPU/memory/disk fields -- only "the Go process is alive,
 * for how long, which commit". The status field is ALWAYS "ok" while the
 * process lives, so it is NOT a health verdict; the verdict is
 * "HTTP 200 + JSON parsed". That is pinned in the code below.
 */
#pragma once

#include <stdint.h>

namespace health {

// One-time setup. Does NOT connect: connecting blocks for seconds and would
// stall the first screen paint. Call update() from loop().
void begin();

// Advance the state machine. Cheap when nothing is due: one millis() compare.
// Safe to call every iteration.
void update();

// True when WiFi is associated. Necessary but NOT sufficient: an AP can
// accept the association and still give no route out of the LAN, which
// makes every HTTPS attempt fail identically (bench-measured 2026-10-17).
// Flips drive the "WiFi lost" buzzer edge (docs/adr/0011) and the ESP32
// screen's wifi line.
bool netOnline();

// True only when the association has been proven to carry traffic out of
// the LAN. This is the precondition for polling: when false, update()
// skips the poll instead of burning a TLS timeout every interval.
bool linkUsable();

// True when data is considered stale: either no successful poll yet, or the
// last successful poll is older than STALE_AFTER_MS.
bool stale();

// Seconds since the last successful poll. Reported on the serial log and the
// server screen so "how stale" is visible, not just "is stale".
uint32_t secondsSinceData();

// Seconds the server process has been up, from its own /api/health "uptime"
// field. 0 when unknown. Displayed on the server screen at M1a.
const char* serverUptime();

// Commit identity from /api/health "commit" (and "built"). Returns "" when
// unavailable. "dev"/"unknown" mean the server binary was built without
// -ldflags injection (docs/adr/0010 edge case 5) -- the caller prints them
// as "not injected" rather than as a real commit.
const char* serverCommit();
const char* serverBuilt();

// Number of successful polls since boot. Serial diagnostics only.
uint32_t pollCount();

// Human-readable one-liner for the serial log, ASCII only (a serial log is
// the only clue available when the screens say nothing useful).
void printStatus();

}  // namespace health
