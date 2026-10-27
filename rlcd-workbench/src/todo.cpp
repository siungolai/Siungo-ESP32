// rlcd-workbench / 待办与行程的实现（M8，ADR-0020）
//
// 与 snapshot.cpp 的区别有三条，且都是刻意的：
//   1. 不用 HTTPClient：这份响应有上界（2048）且必须「超限立刻断开」，
//      HTTPClient 的 getString() 会先把整包读进 String 再交给我们 —— 那等于
//      把「限制长度」这件事放在分配之后。这里自己发请求、自己按上限读。
//   2. 失败不清旧数据：解析先写进临时 Data，全部校验通过才整体换上去。
//   3. 认证/部署类错误不停止轮询，只是把周期拉长到 30 分钟 —— 服务端把
//      key 配好、把端点部署上去之后，设备应该自己恢复，而不是等人重烧固件。

#include "todo.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "jsonfield.h"
#include "timeutil.h"
#include "todo_parse.h"
#include "version.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef TODO_DEVICE_KEY
#define TODO_DEVICE_KEY ""
#endif

namespace todo {
namespace {

Data g_data;
char g_body[cfg::kTodoResponseMax];
char g_items[cfg::kTodoResponseMax];
char g_tmp[512];

uint32_t g_nextPollMs = 0;
uint32_t g_intervalMs = cfg::kTodoPollMs;
uint32_t g_failStreak = 0;
bool g_force = false;
bool g_loggedNoToday = false;
bool g_loggedKey = false;
bool g_loggedUpgrade = false;
bool g_loggedIdentity = false;

void logLine(const char *fmt, ...) {
  char buf[176];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// ---------------- HTTP ----------------

enum class Net : uint8_t {
  Ok = 0,
  Connect,  // TCP/TLS 没连上
  Timeout,  // 超过总时限
  Proto,    // 状态行/头读不动
  TooBig,   // 响应超过 kTodoResponseMax
};

bool deadlineHit(uint32_t deadline) { return (int32_t)(millis() - deadline) > 0; }

// 头名比较与值查找都不带库依赖：strcasecmp/strcasestr 不是标准 C 的一部分，
// 在这个工具链里能不能用要看 newlib 的编译开关 —— 自己写十来行更稳。
bool startsWithNoCase(const char *s, const char *prefix) {
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    char a = s[i];
    char b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool containsNoCase(const char *s, const char *needle) {
  if (needle[0] == '\0') return true;
  for (const char *p = s; *p != '\0'; ++p) {
    if (startsWithNoCase(p, needle)) return true;
  }
  return false;
}

bool readLine(WiFiClientSecure &c, char *out, size_t cap, uint32_t deadline) {
  if (cap == 0) return false;
  size_t n = 0;
  while (true) {
    if (deadlineHit(deadline)) return false;
    const int avail = c.available();
    if (avail <= 0) {
      if (!c.connected()) return false;
      delay(2);
      continue;
    }
    const int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') break;
    if (ch == '\r') continue;
    if (n + 1 < cap) out[n++] = (char)ch;
  }
  out[n] = '\0';
  return true;
}

bool readExact(WiFiClientSecure &c, char *dst, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if (deadlineHit(deadline)) return false;
    const int avail = c.available();
    if (avail <= 0) {
      if (!c.connected()) return false;
      delay(2);
      continue;
    }
    const size_t want = ((size_t)avail < (n - got)) ? (size_t)avail : (n - got);
    const int r = c.readBytes(dst + got, want);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// 发一个 GET、读状态码与头、把 body 按上限读进 out。
// 不信任 Content-Length：chunked 走解码路径，没有长度就边读边数，
// 一旦超过 cap 立刻返回 TooBig（不做「读到内存里再看」那一步）。
Net httpGet(char *out, size_t cap, size_t *outLen, int *outCode) {
  *outLen = 0;
  *outCode = 0;

  WiFiClientSecure client;
  client.setInsecure();  // 与 snapshot.cpp 同取舍（ADR-0006）：不校验证书
  client.setHandshakeTimeout(5);

  const uint32_t t0 = millis();
  const uint32_t deadline = t0 + cfg::kHttpTimeoutMs;

  if (!client.connect(cfg::kDataHost, 443, cfg::kTodoConnectTimeoutMs)) {
    return Net::Connect;
  }

  char head[256];
  const int n = snprintf(head, sizeof(head),
                         "GET %s HTTP/1.1\r\nHost: %s\r\n"
                         "User-Agent: rlcd-workbench/%s\r\n"
                         "Accept: application/json\r\nConnection: close\r\n",
                         cfg::kTodoPath, cfg::kDataHost, ver::kVersion);
  if (n <= 0 || (size_t)n >= sizeof(head)) {
    client.stop();
    return Net::Proto;
  }
  client.print(head);
  const char *key = TODO_DEVICE_KEY;
  if (key[0] != '\0') {
    client.print(cfg::kTodoKeyHeader);
    client.print(": ");
    client.print(key);
    client.print("\r\n");
  }
  client.print("\r\n");

  // ---- 状态行 ----
  char line[128];
  if (!readLine(client, line, sizeof(line), deadline)) {
    client.stop();
    return Net::Timeout;
  }
  if (strncmp(line, "HTTP/", 5) != 0) {
    client.stop();
    return Net::Proto;
  }
  const char *sp = strchr(line, ' ');
  const int code = (sp != nullptr) ? atoi(sp + 1) : 0;
  *outCode = code;

  // ---- 头 ----
  long contentLen = -1;
  bool chunked = false;
  while (true) {
    if (!readLine(client, line, sizeof(line), deadline)) {
      client.stop();
      return Net::Timeout;
    }
    if (line[0] == '\0') break;  // 空行 = 头结束
    if (startsWithNoCase(line, "Content-Length:")) {
      contentLen = strtol(line + 15, nullptr, 10);
    } else if (startsWithNoCase(line, "Transfer-Encoding:")) {
      if (containsNoCase(line + 18, "chunked")) chunked = true;
    }
  }

  // 非 200 也可能带 body（{"error":...}）：照样读一小段，便于日志里看到原因。
  const size_t bodyCap = (cap > 0) ? cap - 1 : 0;

  if (chunked) {
    size_t total = 0;
    while (true) {
      if (!readLine(client, line, sizeof(line), deadline)) {
        client.stop();
        return Net::Timeout;
      }
      const long sz = strtol(line, nullptr, 16);
      if (sz <= 0) break;
      if (total + (size_t)sz > bodyCap) {
        client.stop();
        return Net::TooBig;
      }
      if (!readExact(client, out + total, (size_t)sz, deadline)) {
        client.stop();
        return Net::Timeout;
      }
      total += (size_t)sz;
      char crlf[2];
      readExact(client, crlf, 2, deadline);  // chunk 末尾的 CRLF
    }
    out[total] = '\0';
    *outLen = total;
  } else if (contentLen >= 0) {
    if ((size_t)contentLen > bodyCap) {
      client.stop();
      return Net::TooBig;
    }
    if (!readExact(client, out, (size_t)contentLen, deadline)) {
      client.stop();
      return Net::Timeout;
    }
    out[contentLen] = '\0';
    *outLen = (size_t)contentLen;
  } else {
    size_t total = 0;
    while (total < bodyCap) {
      if (deadlineHit(deadline)) {
        client.stop();
        return Net::Timeout;
      }
      if (client.available() <= 0) {
        if (!client.connected()) break;
        delay(2);
        continue;
      }
      const int r = client.read((uint8_t *)out + total, bodyCap - total);
      if (r <= 0) break;
      total += (size_t)r;
    }
    if (total >= bodyCap) {
      client.stop();
      return Net::TooBig;
    }
    out[total] = '\0';
    *outLen = total;
  }

  client.stop();
  return Net::Ok;
}

// ---------------- 解析 ----------------

// "YYYY-MM-DD"（十位、带横线）。today 与 dueDate 都是这个形状。
bool parseDate(const char *s, int *y, int *mo, int *d) {
  if (s == nullptr || s[0] == '\0') return false;
  if (strlen(s) < 10) return false;
  if (sscanf(s, "%d-%d-%d", y, mo, d) != 3) return false;
  if (*mo < 1 || *mo > 12) return false;
  if (*d < 1 || *d > 31) return false;
  return true;
}

void parseTrips(const char *root, Data &t) {
  if (!todojson::object(root, "trips", g_tmp, sizeof(g_tmp))) return;  // 当月无行程：整个字段省略
  char buf[256];
  jsonfield::field(g_tmp, "month", buf, sizeof(buf));
  // "YYYY-MM" 是 7 字符 + NUL，正好塞满 tripsMonth[8]。比这长的不认 ——
  // 宁可整月不标点（月历上的点本来就是加料），也不拿一个被截断的月份去比。
  const size_t mlen = strlen(buf);
  if (mlen > 0 && mlen < sizeof(t.tripsMonth)) {
    memcpy(t.tripsMonth, buf, mlen + 1);
  }

  if (!todojson::array(g_tmp, "days", buf, sizeof(buf))) return;
  size_t pos = 0;
  long day = 0;
  while (todojson::nextInt(buf, &pos, &day)) {
    if (day >= 1 && day <= 31 && t.tripCount[day] < kDotsPerDay) {
      ++t.tripCount[day];
    }
  }
}

// 把 body 解析进 t。返回 false 表示这一轮不能用（调用方保留旧数据）。
// 只做「能不能用」的判断，不碰 g_data —— 半份新数据比旧数据更坏。
bool parseDocument(const char *body, size_t len, int code, Data &t) {
  if (len < 2 || body[0] != '{') return false;

  // stats：契约里必有。pending 缺失说明这不是待办摘要。
  if (!todojson::object(body, "stats", g_tmp, sizeof(g_tmp))) return false;
  double v = 0;
  if (!jsonfield::number(g_tmp, "pending", &v)) return false;
  t.pending = (int)v;
  if (jsonfield::number(g_tmp, "overdue", &v)) t.overdue = (int)v;
  if (jsonfield::number(g_tmp, "todayDue", &v)) t.todayDue = (int)v;
  if (jsonfield::number(g_tmp, "earlier", &v)) t.earlier = (int)v;
  if (jsonfield::number(g_tmp, "undated", &v)) t.undated = (int)v;
  if (jsonfield::number(g_tmp, "outOfWindow", &v)) t.outOfWindow = (int)v;
  if (t.pending < 0) t.pending = 0;

  // items：契约里必有（哪怕是个空数组）。切不出来说明响应坏了 —— 直接拒绝这一轮，
  // 不要退回「当 0 条渲染」：那会把一次解析失败伪装成「今天没有待办」。
  if (!todojson::array(body, "items", g_items, sizeof(g_items))) return false;

  char item[512];
  char title[kTextCap];
  char due[kDueCap];
  size_t pos = 0;
  int seen = 0;
  int n = 0;
  while (todojson::nextObject(g_items, &pos, item, sizeof(item))) {
    if (++seen > kSlots) return false;  // 契约上限 6 条，多了说明服务端坏了
    jsonfield::field(item, "title", title, sizeof(title));
    jsonfield::field(item, "dueDate", due, sizeof(due));
    if (title[0] == '\0') {
      ++t.badRows;  // 有形状没内容：跳过这一条并计数，其余照常
      continue;
    }
    snprintf(t.text[n], sizeof(t.text[n]), "%s", title);
    snprintf(t.due[n], sizeof(t.due[n]), "%s", due);
    ++n;
  }
  t.slots = n;
  if (t.pending < n) return false;  // 恒等式的一半：屏上行数不可能多于未完成总数

  // today：下一版契约才有的字段。没有就留空，由 relativeDue 退回本机日期。
  char today[kTodayCap];
  jsonfield::field(body, "today", today, sizeof(today));
  int y = 0, mo = 0, d = 0;
  if (parseDate(today, &y, &mo, &d)) {
    snprintf(t.today, sizeof(t.today), "%s", today);
  } else if (!g_loggedNoToday) {
    g_loggedNoToday = true;
    logLine("[todo] today absent, falling back to local date");
  }

  if (jsonfield::number(body, "remaining", &v)) t.remaining = (int)v;
  if (jsonfield::hasKey(body, "truncated")) t.truncated = true;

  parseTrips(body, t);

  // 恒等式自检（下一版契约的字段齐了才有意义）：
  // pending == undated + outOfWindow + len(items) + remaining
  if (t.undated >= 0 && t.outOfWindow >= 0) {
    const int sum = t.undated + t.outOfWindow + t.slots + t.remaining;
    if (sum != t.pending && !g_loggedIdentity) {
      g_loggedIdentity = true;
      logLine("[todo] stats identity mismatch: %d != %d (undated=%d outOfWindow=%d items=%d "
              "remaining=%d)",
              sum, t.pending, t.undated, t.outOfWindow, t.slots, t.remaining);
    }
  }

  t.lastHttpCode = code;
  t.lastLen = (uint32_t)len;
  return true;
}

bool fetchOnce() {
  size_t len = 0;
  int code = 0;
  const Net net = httpGet(g_body, sizeof(g_body), &len, &code);
  if (net != Net::Ok) {
    ++g_data.failCount;
    g_data.lastHttpCode = (net == Net::TooBig) ? -2 : -1;
    if (net == Net::TooBig) {
      logLine("[todo] response over %u bytes - dropped whole round",
              (unsigned)cfg::kTodoResponseMax);
    }
    return false;
  }

  g_data.lastHttpCode = code;
  g_data.lastLen = (uint32_t)len;

  if (code != 200) {
    ++g_data.failCount;
    g_data.keyRejected = (code == 401);
    g_data.endpointMissing = (code == 404);
    if (code == 401 && !g_loggedKey) {
      g_loggedKey = true;
      // 401 是三态同形：key 错、没带、服务端没配。文档明确说它不是「你配错了」的信号。
      logLine("[todo] HTTP 401 - key wrong/absent/not configured on server");
    } else if (code != 401) {
      logLine("[todo] HTTP %d (%.40s)", code, g_body);
    }
    return false;
  }

  // schemaVersion 在整份文档里只出现一次，可以直接在顶层找。
  double sv = 0;
  if (!jsonfield::number(g_body, "schemaVersion", &sv)) {
    ++g_data.failCount;
    logLine("[todo] schemaVersion missing (head=%.32s)", g_body);
    return false;
  }
  if ((int)sv != 1) {
    ++g_data.failCount;
    g_data.upgradeRequired = true;
    if (!g_loggedUpgrade) {
      g_loggedUpgrade = true;
      logLine("[todo] schemaVersion=%d, firmware needs upgrade", (int)sv);
    }
    return false;
  }

  Data t;
  if (!parseDocument(g_body, len, code, t)) {
    ++g_data.failCount;
    logLine("[todo] document rejected (len=%u, head=%.32s)", (unsigned)len, g_body);
    return false;
  }

  // 到这里才整体换新：上面任何一步失败，屏上仍然是上一份好数据。
  const uint32_t fails = g_data.failCount;
  const uint32_t polls = g_data.pollCount + 1;
  g_data = t;
  g_data.valid = true;
  g_data.fetchedMs = millis();
  g_data.pollCount = polls;
  g_data.failCount = fails;
  g_data.upgradeRequired = false;
  g_data.keyRejected = false;
  g_data.endpointMissing = false;

  static bool loggedKeys = false;
  if (!loggedKeys) {
    loggedKeys = true;
    logLine("[todo] ok: pending=%d overdue=%d items=%d remaining=%d today=%s", g_data.pending,
            g_data.overdue, g_data.slots, g_data.remaining,
            g_data.today[0] ? g_data.today : "-");
  }
  return true;
}

uint32_t nextPollInterval() {
  // 认证/部署类错误：不是「等网络恢复」，是「等人去改服务端」。但也不能真的
  // 停死 —— 服务端修好后设备应该自己回来。30 分钟一次慢探，代价接近零。
  if (g_data.keyRejected || g_data.endpointMissing) return cfg::kTodoSlowProbeMs;

  const uint32_t cap = cfg::kTodoBackoffMaxMs;
  uint32_t v = cfg::kTodoBackoffMinMs;
  for (uint32_t i = 0; i + 1 < g_failStreak && v < cap; ++i) {
    v *= 2;
  }
  return v > cap ? cap : v;
}

}  // namespace

void begin() {
  // 不在 begin() 里立刻拉：那时候 WiFi 还没起来，一次连接要等满超时，
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
      logLine("[todo] recovered after %u fail(s), back to %u ms", (unsigned)g_failStreak,
              (unsigned)cfg::kTodoPollMs);
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

bool relativeDue(const char *dueDate, const char *today, int year, int month, int day,
                 char *out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';

  int y = 0, mo = 0, d = 0;
  if (!parseDate(dueDate, &y, &mo, &d)) return false;

  // 「今天」以服务端为准；没发才退回本机日期（宽进）。
  int by = year, bm = month, bd = day;
  int ty = 0, tm = 0, td = 0;
  if (parseDate(today, &ty, &tm, &td)) {
    by = ty;
    bm = tm;
    bd = td;
  }

  const int64_t a = tu::daysFromCivil(y, (unsigned)mo, (unsigned)d);
  const int64_t b = tu::daysFromCivil(by, (unsigned)bm, (unsigned)bd);
  const int64_t diff = a - b;

  if (diff < 0) {
    snprintf(out, cap, "逾期%lld天", (long long)(-diff));
  } else if (diff == 0) {
    snprintf(out, cap, "今天");
  } else if (diff == 1) {
    snprintf(out, cap, "明天");
  } else if (diff <= 7) {
    snprintf(out, cap, "%lld天后", (long long)diff);
  } else {
    // 再远就写日期。到期栏只有 23–42 px，正文会让位（见 ui 的像素裁切）。
    snprintf(out, cap, "%02d-%02d", mo, d);
  }
  return true;
}

}  // namespace todo
