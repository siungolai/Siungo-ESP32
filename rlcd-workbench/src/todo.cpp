// rlcd-workbench / 待办与行程的实现（M3）
//
// 与 snapshot.cpp 共用同一套取舍：不做证书校验、失败不清旧数据、403 单独报
// 一行、双重验收（既像 JSON 又带 generated_at）。区别只有一条 —— 这份文档
// 允许「某一条内容坏了」，坏了要能被数出来，而不是整份作废。

#include "todo.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "jsonfield.h"
#include "timeutil.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef MONITOR_KEY
#define MONITOR_KEY ""
#endif

namespace todo {
namespace {

Data g_data;
char g_body[cfg::kHttpResponseMax];
bool g_force = false;
uint32_t g_nextPollMs = 0;
uint32_t g_intervalMs = cfg::kTodoPollMs;
uint32_t g_failStreak = 0;
bool g_everLoggedFail = false;

void logLine(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// 把 "YYYY-MM-DD HH:MM" 拆开。返回 false 表示格式不认 —— 这条会被跳过。
bool parseDue(const char *s, int *y, int *mo, int *d, int *h, int *mi) {
  if (s == nullptr) return false;
  // 允许 "YYYY-MM-DD"（没有时间）与 "YYYY-MM-DD HH:MM" 两种。
  const int n = sscanf(s, "%d-%d-%d %d:%d", y, mo, d, h, mi);
  if (n == 3) {
    *h = 23;
    *mi = 59;
    return true;
  }
  if (n != 5) return false;
  if (*mo < 1 || *mo > 12 || *d < 1 || *d > 31) return false;
  if (*h < 0 || *h > 23 || *mi < 0 || *mi > 59) return false;
  return true;
}

void parseDays(const char *s) {
  memset(g_data.tripCount, 0, sizeof(g_data.tripCount));
  if (s == nullptr || s[0] == '\0') return;
  // "22:2,24:1,30:1"
  const char *p = s;
  while (*p != '\0') {
    int day = 0, cnt = 0;
    const int n = sscanf(p, "%d:%d", &day, &cnt);
    if (n == 2 && day >= 1 && day <= 31 && cnt > 0) {
      g_data.tripCount[day] = (uint8_t)(cnt > 255 ? 255 : cnt);
    }
    // 跳过这一对
    const char *comma = strchr(p, ',');
    if (comma == nullptr) break;
    p = comma + 1;
  }
}

bool fetchOnce() {
  WiFiClientSecure client;
  client.setInsecure();  // 与 snapshot.cpp 同取舍
  client.setTimeout(cfg::kHttpTimeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(cfg::kHttpTimeoutMs);
  http.setReuse(false);

  String url = String("https://") + cfg::kDataHost + cfg::kTodoPath;
  if (!http.begin(client, url)) {
    ++g_data.failCount;
    return false;
  }
  const char *key = MONITOR_KEY;
  if (key[0] != '\0') {
    http.addHeader(cfg::kMonitorKeyHeader, key);
  }

  const int code = http.GET();
  g_data.lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    http.end();
    ++g_data.failCount;
    if (code == 403 && !g_everLoggedFail) {
      logLine("[todo] HTTP 403 - monitor key rejected or nginx whitelist not updated");
      g_everLoggedFail = true;
    }
    return false;
  }

  const String body = http.getString();
  http.end();

  const size_t n = body.length();
  g_data.lastLen = (uint32_t)n;
  if (n == 0 || n >= sizeof(g_body)) {
    logLine("[todo] body length %u out of range (buffer %u)", (unsigned)n,
            (unsigned)sizeof(g_body) - 1);
    ++g_data.failCount;
    return false;
  }
  memcpy(g_body, body.c_str(), n);
  g_body[n] = '\0';

  if (g_body[0] != '{' || !jsonfield::hasKey(g_body, "generated_at")) {
    logLine("[todo] body is not a todo document (head=%.32s)", g_body);
    ++g_data.failCount;
    return false;
  }

  double v = 0;
  g_data.total = jsonfield::number(g_body, "total", &v) ? (int)v : 0;
  g_data.done = jsonfield::number(g_body, "done", &v) ? (int)v : 0;
  if (g_data.total < 0) g_data.total = 0;
  if (g_data.done < 0) g_data.done = 0;
  if (g_data.done > g_data.total) g_data.done = g_data.total;

  memset(g_data.text, 0, sizeof(g_data.text));
  memset(g_data.due, 0, sizeof(g_data.due));
  g_data.slots = 0;
  g_data.badRows = 0;

  for (int i = 0; i < kSlots; ++i) {
    char kText[16], kDue[16];
    snprintf(kText, sizeof(kText), "item%d_text", i + 1);
    snprintf(kDue, sizeof(kDue), "item%d_due", i + 1);
    jsonfield::field(g_body, kText, g_data.text[i], kTextCap);
    jsonfield::field(g_body, kDue, g_data.due[i], kDueCap);
    // 有一条算一条；空槽不占位也不报错（条目不够 6 条是正常的）。
    if (g_data.text[i][0] != '\0') {
      ++g_data.slots;
    }
    // 有正文但到期串写了却不是日期 —— 这一条照样显示，只是不显示到期。
    // 不把它算作坏行：「今天到期」和「到期字段写错」对看屏的人是同一件事，
    // 而正文本身是有用的。真正的坏行是正文为空却有到期。
    if (g_data.text[i][0] == '\0' && g_data.due[i][0] != '\0') {
      ++g_data.badRows;
    }
  }

  jsonfield::field(g_body, "days_month", g_data.daysMonth, sizeof(g_data.daysMonth));
  char daysBuf[96];
  jsonfield::field(g_body, "days", daysBuf, sizeof(daysBuf));
  parseDays(daysBuf);

  static bool g_loggedKeys = false;
  if (!g_loggedKeys) {
    g_loggedKeys = true;
    // 这里打的是「字段在不在」而不是值 —— 值的检查交给下面这行汇总。
    // 名字写清楚，否则 hasTotal=1 很容易被读成 total=1。
    logLine("[todo] keys: hasTotal=%d hasDone=%d slots=%d total=%d done=%d daysMonth=%s days=%s",
            (int)jsonfield::hasKey(g_body, "total"),
            (int)jsonfield::hasKey(g_body, "done"),
            g_data.slots, g_data.total, g_data.done,
            g_data.daysMonth[0] ? g_data.daysMonth : "-", daysBuf[0] ? daysBuf : "-");
  }

  g_data.valid = true;
  g_data.fetchedMs = millis();
  ++g_data.pollCount;
  return true;
}

uint32_t nextPollInterval() {
  // 与快照同一套退避：300 → 600 → 1200 s 封顶。待办本来就是 5 分钟一次，
  // 失败后拉长到 20 分钟不算过分；恢复后立刻回到 300 s。
  const uint32_t base = cfg::kTodoPollMs;
  const uint32_t cap = cfg::kTodoBackoffMaxMs;
  // 第一次失败仍按原周期重试，不要一上来就翻倍。
  // 开机时 WiFi 还没连上，第一轮必然失败 —— 那时候把 5 分钟周期直接翻成 10 分钟，
  // 就会在「网明明已经连上」之后还干等十分钟才去取第一次数据。
  uint32_t v = base;
  for (uint32_t i = 0; i + 1 < g_failStreak && v < cap; ++i) {
    v *= 2;
  }
  return v > cap ? cap : v;
}

}  // namespace

void begin() {
  // 不在 begin() 里立刻拉：那时候 WiFi 还没起来，一次 HTTP 连接要等满超时，
  // 白白把开机拖长几秒。给 5 秒 —— 这块板的 WiFi 实测 3.3 秒连上。
  g_nextPollMs = millis() + 5000;
  g_intervalMs = cfg::kTodoPollMs;
}

void requestRefresh() { g_force = true; }

void tick() {
  const uint32_t now = millis();
  if (!g_force && (int32_t)(now - g_nextPollMs) < 0) {
    return;
  }
  g_force = false;

  const bool ok = fetchOnce();
  if (ok) {
    if (g_failStreak > 0) {
      logLine("[todo] recovered after %u fail(s), back to %u ms",
              (unsigned)g_failStreak, (unsigned)cfg::kTodoPollMs);
    }
    g_failStreak = 0;
    g_intervalMs = cfg::kTodoPollMs;
  } else {
    ++g_failStreak;
    g_intervalMs = nextPollInterval();
    logLine("[todo] fail #%u (http=%d), next try in %u ms", (unsigned)g_failStreak,
            g_data.lastHttpCode, (unsigned)g_intervalMs);
  }
  g_nextPollMs = now + g_intervalMs;
}

const Data &data() { return g_data; }

uint32_t ageMs() { return g_data.valid ? (millis() - g_data.fetchedMs) : 0; }

bool stale() { return g_data.valid && ageMs() > cfg::kTodoStaleMs; }

bool fresh() { return g_data.valid && !stale(); }

bool relativeDue(const char *due, int year, int month, int day, int hour, int minute,
                 char *out, size_t cap) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (!parseDue(due, &y, &mo, &d, &h, &mi)) {
    if (out != nullptr && cap > 0) out[0] = '\0';
    return false;
  }
  const int64_t a = tu::daysFromCivil(y, (unsigned)mo, (unsigned)d);
  const int64_t b = tu::daysFromCivil(year, (unsigned)month, (unsigned)day);
  const int64_t diffDays = a - b;

  if (diffDays < 0) {
    snprintf(out, cap, "逾期");
    return true;
  }
  if (diffDays == 0) {
    const int dueMin = h * 60 + mi;
    const int nowMin = hour * 60 + minute;
    if (dueMin < nowMin) {
      snprintf(out, cap, "逾期");
    } else if (dueMin - nowMin >= 60) {
      snprintf(out, cap, "%dh", (dueMin - nowMin) / 60);
    } else {
      snprintf(out, cap, "今天");
    }
    return true;
  }
  if (diffDays == 1) {
    snprintf(out, cap, "明天");
    return true;
  }
  // 再远就只留 3 个字：到期栏的预算是 23 px（"明天"正好 23），"10-28" 要 28 px
  // 会把正文挤掉一截。同月写日，跨月写月 —— 反正精确日期正文里也不显示。
  if (mo == month) {
    snprintf(out, cap, "%d日", d);
  } else {
    snprintf(out, cap, "%d月", mo);
  }
  return true;
}

}  // namespace todo
