#include "snapshot.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jsonfield.h"
#include "net_wifi.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define MONITOR_KEY ""
#endif

namespace snapshot {
namespace {

Data g_data;
uint32_t g_lastAttemptMs = 0;
bool g_force = false;
bool g_everLoggedFail = false;

char g_body[cfg::kHttpResponseMax + 1];

void logLine(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[192];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

Probe probeFromField(const char *key) {
  double v = 0;
  if (!jsonfield::number(g_body, key, &v)) {
    return Probe::Unknown;  // 字段缺失或为 null —— 不是失败
  }
  return (v >= 1.0) ? Probe::Ok : Probe::Fail;
}

// 把 KB 折成人类看得懂的字符串。
// 内存用 MB 整数（本机 1682 MB，用 GB 一位小数一档就是 102 MB ≈ 6 %，太糊）；
// 磁盘用 GB 一位小数（40 GB 写成 40960 M 只是噪音）。单位由量级决定。
void formatMem(char *out, size_t cap, double totalKb, double usedKb, double freeKb) {
  snprintf(out, cap, "%dM/%dM/%dM", (int)lround(usedKb / 1024.0),
           (int)lround(freeKb / 1024.0), (int)lround(totalKb / 1024.0));
}

void formatDisk(char *out, size_t cap, double totalKb, double usedKb, double availKb) {
  snprintf(out, cap, "%.1fG/%.1fG/%.1fG", usedKb / 1024.0 / 1024.0, availKb / 1024.0 / 1024.0,
           totalKb / 1024.0 / 1024.0);
}

bool fetchOnce() {
  WiFiClientSecure client;
  // 不做证书校验：这块板只读一个自己服务器的公开监控文件，且上 TLS 会再占
  // 一份证书包（Flash 已经很紧）。同 status-oled3 的取舍。
  client.setInsecure();
  client.setTimeout(cfg::kHttpTimeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(cfg::kHttpTimeoutMs);
  http.setReuse(false);

  String url = String("https://") + cfg::kDataHost + cfg::kSnapshotPath;
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
    // 403 几乎总是「服务器还没把白名单放行」，单独报出来能省一整轮排查。
    if (code == 403 && !g_everLoggedFail) {
      logLine("[snap] HTTP 403 - monitor key rejected or nginx whitelist not updated");
      g_everLoggedFail = true;
    }
    return false;
  }

  const String body = http.getString();
  http.end();

  const size_t n = body.length();
  g_data.lastLen = (uint32_t)n;
  if (n == 0 || n >= sizeof(g_body)) {
    logLine("[snap] body length %u out of range (buffer %u)", (unsigned)n,
            (unsigned)sizeof(g_body) - 1);
    ++g_data.failCount;
    return false;
  }
  memcpy(g_body, body.c_str(), n);
  g_body[n] = '\0';

  // 双重验收：既要像 JSON，又必须带 generated_at。
  // 只看长度或只看括号会把 nginx 的错误页当成快照。
  if (g_body[0] != '{' || !jsonfield::hasKey(g_body, "generated_at")) {
    logLine("[snap] body is not a snapshot (head=%.32s)", g_body);
    ++g_data.failCount;
    return false;
  }

  double v = 0;

  // ---- 左栏 ----
  g_data.hasCpu = jsonfield::number(g_body, "cpu_pct", &v);
  if (g_data.hasCpu) g_data.cpuPct = (int)v;

  double memTotal = 0, memUsed = 0, memFree = 0;
  const bool memOk = jsonfield::number(g_body, "mem_total_kb", &memTotal) &&
                     jsonfield::number(g_body, "mem_used_kb", &memUsed) &&
                     jsonfield::number(g_body, "mem_free_kb", &memFree);
  g_data.hasMem = memOk && memTotal > 0;
  if (g_data.hasMem) {
    g_data.memPct = (int)lround(memUsed * 100.0 / memTotal);
    formatMem(g_data.memDetail, sizeof(g_data.memDetail), memTotal, memUsed, memFree);
  }

  double diskTotal = 0, diskUsed = 0, diskAvail = 0;
  const bool diskOk = jsonfield::number(g_body, "disk_total_kb", &diskTotal) &&
                      jsonfield::number(g_body, "disk_used_kb", &diskUsed) &&
                      jsonfield::number(g_body, "disk_avail_kb", &diskAvail);
  g_data.hasDisk = diskOk && diskTotal > 0;
  if (g_data.hasDisk) {
    g_data.diskPct = (int)lround(diskUsed * 100.0 / diskTotal);
    formatDisk(g_data.diskDetail, sizeof(g_data.diskDetail), diskTotal, diskUsed, diskAvail);
  }

  g_data.hasLoad = jsonfield::number(g_body, "load1_x100", &v);
  if (g_data.hasLoad) g_data.load1 = (float)(v / 100.0);

  // ---- 右栏：六个探针 ----
  g_data.svcWeb = probeFromField("svc_web");
  g_data.svcSelect = probeFromField("svc_select");
  g_data.svcWb = probeFromField("svc_wb");
  g_data.svcApi = probeFromField("svc_api");
  g_data.svcUpload = probeFromField("svc_upload");
  g_data.svcMa12 = probeFromField("svc_ma12");

  // ---- 证书 ----
  g_data.hasCert = jsonfield::number(g_body, "cert_days", &v);
  if (g_data.hasCert) g_data.certDays = (int)v;

  // 首份快照把关键字段的存在性打一遍 —— 「服务器还没部署这几个字段」和
  // 「设备解析写错了」在屏上都表现为问号，这一行能一次性分清是哪种。
  static bool g_loggedKeys = false;
  if (!g_loggedKeys) {
    g_loggedKeys = true;
    logLine("[snap] keys: web=%d select=%d wb=%d api=%d upload=%d ma12=%d cert=%d",
            (int)jsonfield::hasKey(g_body, "svc_web"),
            (int)jsonfield::hasKey(g_body, "svc_select"),
            (int)jsonfield::hasKey(g_body, "svc_wb"),
            (int)jsonfield::hasKey(g_body, "svc_api"),
            (int)jsonfield::hasKey(g_body, "svc_upload"),
            (int)jsonfield::hasKey(g_body, "svc_ma12"),
            (int)jsonfield::hasKey(g_body, "cert_days"));
  }

  g_data.valid = true;
  g_data.fetchedMs = millis();
  ++g_data.pollCount;
  return true;
}

}  // namespace

void begin() {
  g_lastAttemptMs = millis();
  g_force = true;  // 开机立刻拉一次，不等首个 15 秒周期
}

void requestRefresh() { g_force = true; }

namespace {
// 当前轮询间隔。成功时恒为 kSnapshotPollMs，失败后按 15/30/60/120 阶梯拉长。
uint32_t g_pollIntervalMs = cfg::kSnapshotPollMs;
uint32_t g_failStreak = 0;

uint32_t nextPollInterval() {
  uint32_t ms = cfg::kSnapshotPollMs;
  for (uint32_t i = 0; i < g_failStreak && ms < cfg::kSnapshotBackoffMaxMs; ++i) {
    ms *= 2;
  }
  return ms > cfg::kSnapshotBackoffMaxMs ? cfg::kSnapshotBackoffMaxMs : ms;
}
}  // namespace

void tick() {
  if (netwifi::state() != netwifi::State::Connected) {
    return;  // 没网不发请求，避免每次 tick 都白连一次
  }

  const uint32_t now = millis();
  if (!g_force && (now - g_lastAttemptMs) < g_pollIntervalMs) {
    return;
  }
  g_force = false;
  g_lastAttemptMs = now;

  if (!fetchOnce()) {
    // 失败不清数据：屏幕上宁可显示「60 秒前的读数 + 离线标记」，
    // 也不要一片空白 —— 空白分不清「服务器挂了」和「设备没联网」。
    ++g_failStreak;
    g_pollIntervalMs = nextPollInterval();
    logLine("[snap] fail #%lu (http=%d), next try in %lu ms", (unsigned long)g_failStreak,
            g_data.lastHttpCode, (unsigned long)g_pollIntervalMs);
    return;
  }
  if (g_failStreak > 0) {
    logLine("[snap] recovered after %lu fail(s), back to %lu ms",
            (unsigned long)g_failStreak, (unsigned long)cfg::kSnapshotPollMs);
    g_failStreak = 0;
  }
  g_pollIntervalMs = cfg::kSnapshotPollMs;

  char ps[8];
  const Probe pp[6] = {g_data.svcWeb,  g_data.svcSelect, g_data.svcWb,
                       g_data.svcApi,  g_data.svcUpload, g_data.svcMa12};
  for (int i = 0; i < 6; ++i) {
    ps[i] = (pp[i] == Probe::Ok) ? 'Y' : (pp[i] == Probe::Fail ? 'N' : '?');
  }
  ps[6] = '\0';

  logLine("[snap] ok len=%u cpu=%d%% mem=%d%% disk=%d%% load=%.2f cert=%d probes=%s",
          (unsigned)g_data.lastLen, g_data.cpuPct, g_data.memPct, g_data.diskPct,
          (double)g_data.load1, g_data.certDays, ps);
  logLine("[snap] mem=%s disk=%s (Y=ok N=fail ?=field missing)", g_data.memDetail,
          g_data.diskDetail);
}

const Data &data() { return g_data; }

uint32_t ageMs() {
  if (!g_data.valid) return UINT32_MAX;
  return millis() - g_data.fetchedMs;
}

bool stale() {
  if (!g_data.valid) return false;
  return ageMs() > cfg::kSnapshotStaleMs;
}

bool fresh() {
  if (!g_data.valid) return false;
  return ageMs() <= cfg::kSnapshotStaleMs;
}

}  // namespace snapshot
