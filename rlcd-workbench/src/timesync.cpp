#include "timesync.h"

#include <stdarg.h>
#include <time.h>

#include "clock.h"
#include "config.h"
#include "net_wifi.h"
#include "rtc.h"
#include "timeutil.h"
#include "tz.h"

namespace timesync {
namespace {

enum class Phase : uint8_t { Idle, Waiting, Done };

Source g_source = Source::None;
bool g_rtcPresent = false;
bool g_rtcValid = false;
bool g_tzValid = false;

Phase g_phase = Phase::Idle;
uint32_t g_startedMs = 0;
uint32_t g_lastSyncMs = 0;
bool g_everSynced = false;
bool g_forceRequest = false;
int g_attempts = 0;

void logLine(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[160];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// 从 RTC 灌初值。只有「振荡器没停过 + 读数合法 + 年份在合理区间」才认 ——
// 换过电池的板子寄存器里常有看起来像时间、其实是上次掉电瞬间的残值。
void seedFromRtc() {
  if (!g_rtcPresent) {
    return;
  }
  int64_t epoch = 0;
  if (!rtc::readUtc(&epoch)) {
    logLine("[time] rtc present but registers are not valid BCD/time");
    return;
  }
  if (rtc::oscillatorStopped()) {
    logLine("[time] rtc holds a value but OS flag says it stopped -> not trusted");
    return;
  }
  if (epoch < cfg::kTimeSanityEpoch) {
    logLine("[time] rtc epoch too old, ignoring");
    return;
  }

  const tu::DT local = tz::toLocal(epoch);
  workclock::begin(local);
  g_rtcValid = true;
  g_source = Source::Rtc;
  logLine("[time] seeded from rtc (utc=%lld) -> local %04d-%02d-%02d %02d:%02d:%02d",
          (long long)epoch, local.year, local.month, local.day, local.hour, local.minute,
          local.second);
}

void adoptNtp(int64_t utc) {
  const tu::DT local = tz::toLocal(utc);
  workclock::begin(local);
  g_source = Source::Ntp;
  g_everSynced = true;
  g_lastSyncMs = millis();
  g_phase = Phase::Done;
  ++g_attempts;

  const bool wrote = rtc::writeUtc(utc);
  g_rtcValid = true;
  logLine("[time] ntp ok utc=%lld -> local %04d-%02d-%02d %02d:%02d:%02d (rtc %s)",
          (long long)utc, local.year, local.month, local.day, local.hour, local.minute,
          local.second, wrote ? "written" : "write failed");
}

void startSync() {
  // configTime 传 0 偏移：SNTP 只负责把系统时钟设成 UTC，本地化我们自己算。
  // 直接传 8*3600 也能跑，但那样一旦换时区就得改代码，而且 DST 无从谈起。
  configTime(0, 0, cfg::kNtpServer1, cfg::kNtpServer2, cfg::kNtpServer3);
  g_phase = Phase::Waiting;
  g_startedMs = millis();
  logLine("[time] sntp request sent");
}

}  // namespace

void begin() {
  g_tzValid = tz::setByName(cfg::kTimeZoneName);
  logLine("[time] zone = %s (%s)", tz::name(), g_tzValid ? "found" : "NOT FOUND -> UTC");

  g_rtcPresent = rtc::begin();
  logLine("[time] rtc %s", g_rtcPresent ? "present" : "MISSING");
  if (g_rtcPresent) {
    seedFromRtc();
  }
}

void requestSync() { g_forceRequest = true; }

void tick() {
  const uint32_t now = millis();

  if (g_phase == Phase::Waiting) {
    const time_t t = time(nullptr);
    if ((int64_t)t >= cfg::kTimeSanityEpoch) {
      adoptNtp((int64_t)t);
      return;
    }
    if (now - g_startedMs >= cfg::kNtpWaitTimeoutMs) {
      g_phase = Phase::Idle;
      ++g_attempts;
      logLine("[time] sntp timeout after %lu ms (attempt %d), will retry later",
              (unsigned long)cfg::kNtpWaitTimeoutMs, g_attempts);
    }
    return;
  }

  if (netwifi::state() != netwifi::State::Connected) {
    return;  // 没网不发起，避免每 100 ms 白试一次
  }

  const bool due = !g_everSynced || (now - g_lastSyncMs >= cfg::kResyncIntervalMs);
  if (g_forceRequest || due) {
    g_forceRequest = false;
    startSync();
  }
}

bool synced() { return g_source != Source::None; }
Source source() { return g_source; }
bool rtcValid() { return g_rtcValid; }
bool rtcPresent() { return g_rtcPresent; }

const char *sourceName() {
  switch (g_source) {
    case Source::Rtc:
      return "rtc";
    case Source::Ntp:
      return "ntp";
    default:
      return "none";
  }
}

}  // namespace timesync
