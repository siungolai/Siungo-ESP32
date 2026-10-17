// status-oled3 firmware — HTTP health poller (M1b)
//
// M1a asked the aggregator for uptime/commit/built. M1b asks for the full
// ADR-0012 snapshot.json (19 fields) plus a second mailbox.json (5 fields).
// Both documents are read with the same 1 KiB response buffer and the same
// text extractor (jsonfield.h), so the firmware carries no JSON parser.
//
// Two documents, two verdicts. A stale snapshot and a stale mailbox are
// different failures with different UX: the server screen can still render
// the last snapshot while the mailbox screen reports "no data". Neither
// document can be used to excuse the other, so their timers, staleness
// thresholds and success counters are all separate.
//
// All 19 snapshot keys are exposed verbatim (generated_at, sys_uptime_s, ...)
// because that is exactly what status-snapshot.py writes; a rename in either
// place must be mirrored in the other and in ADR-0012.

#pragma once

#include <stdint.h>

namespace health {

// Starts the poller: resets every cached value, edge-detection state and
// counter. Does not touch the network (caller brings WiFi up first).
void begin();

// Advances the state machine: WiFi upkeep, link verification, and one poll
// when a document comes due. Never blocks longer than one HTTP request.
void update();

// ---- network state -------------------------------------------------------
// g_net_online and g_link_usable are deliberately separate facts:
// "associated with an AP" is not "able to reach the internet".
bool netOnline();
bool linkUsable();

// True when the newest snapshot is older than config::STALE_AFTER_MS.
// False until the first snapshot lands (no data yet is "waiting", not "stale").
bool stale();
uint32_t secondsSinceData();

// ---- snapshot values (ADR-0012, 19 fields) --------------------------------
// Key names are the literal snapshot.json names. Missing key, null value or
// non-numeric value all yield NAN / "", so callers must test hasNumber()
// before trusting number() — null means "the host could not collect this",
// which is not the same as 0.
bool hasNumber(const char* key);
double number(const char* key);
const char* string(const char* key);

// ---- mailbox document (ADR-0012, 5 fields) --------------------------------
// available: 1 = credentials configured, 0 = mailbox not configured at all.
// unread: cross-account UNSEEN total, or null when any account failed.
bool mailboxAvailable();
bool mailboxUnreadKnown();
long mailboxUnread();
unsigned mailboxAccountCount();
unsigned mailboxFailedCount();
// Separate staleness for the mailbox document (see header comment).
bool mailboxStale();
uint32_t mailboxSecondsSinceData();

// True on the single call where unread went up versus the previous poll.
// Only meaningful when mailboxUnreadKnown(); drives the new-mail buzzer.
bool unreadRose();

// Diagnostics: how many times each document was fetched successfully.
uint32_t pollCount();
uint32_t mailboxPollCount();

// One-line serial summary (net/link/staleness/age/counters).
void printStatus();

}  // namespace health
