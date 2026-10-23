#include "net_wifi.h"

#include <WiFi.h>

#include "config.h"

// 凭据只来自 gitignored 的 secrets.h。没这个文件、或里面没填，就按
// 「未配置」处理 —— 缺文件不能导致编译失败，否则第一次拿到工程的人
// 连 hello world 都烧不进去。
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

namespace netwifi {
namespace {

// 内部四阶段，比对外三态多一个 Waiting：连接中与「失败后等重试」在屏幕上
// 都画成半格，但内部行为不同 —— 前者在计时超时，后者在等退避。
enum class Phase : uint8_t { Idle, Trying, Waiting, Up };

Phase g_phase = Phase::Idle;
bool g_configured = false;
uint32_t g_attemptStartMs = 0;
uint32_t g_waitStartMs = 0;
uint32_t g_backoffMs = cfg::kWifiBackoffMinMs;
uint32_t g_connectCount = 0;
uint32_t g_dropCount = 0;
uint8_t g_failCount = 0;
int g_rssi = 0;
char g_ip[16] = {};

bool credentialsUsable() {
  const char *s = WIFI_SSID;
  if (s[0] == '\0') {
    return false;
  }
  // secrets.h.example 里的占位串，照抄忘改时不要连，也不要刷屏
  if (strcmp(s, "your-ssid") == 0) {
    return false;
  }
  return true;
}

uint32_t nextBackoff(uint8_t fails) {
  uint32_t ms = cfg::kWifiBackoffMinMs;
  for (uint8_t i = 1; i < fails && ms < cfg::kWifiBackoffMaxMs; ++i) {
    ms *= 2;
  }
  return ms > cfg::kWifiBackoffMaxMs ? cfg::kWifiBackoffMaxMs : ms;
}

void enterUp() {
  g_phase = Phase::Up;
  g_failCount = 0;
  g_backoffMs = cfg::kWifiBackoffMinMs;
  g_rssi = WiFi.RSSI();
  const IPAddress ip = WiFi.localIP();
  snprintf(g_ip, sizeof(g_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  ++g_connectCount;
  // 串口只用 ASCII（仓库约定）
  Serial.printf("[wifi] connected ssid=%s ip=%s rssi=%d dBm (%lu ms since boot)\n", WIFI_SSID,
                g_ip, g_rssi, (unsigned long)millis());
}

void startAttempt(const char *why) {
  g_phase = Phase::Trying;
  g_attemptStartMs = millis();
  Serial.printf("[wifi] %s -> begin (attempt %u)\n", why, (unsigned)(g_failCount + 1));
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

}  // namespace

void begin() {
  if (!credentialsUsable()) {
    g_configured = false;
    g_phase = Phase::Idle;
    Serial.println("[wifi] not configured (fill WIFI_SSID/WIFI_PASS in src/secrets.h)");
    return;
  }
  g_configured = true;

  WiFi.persistent(false);      // 不把凭据反复写进 NVS，省 Flash 寿命
  WiFi.setAutoReconnect(false);  // 重连由本模块自己管，见文件头
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("rlcd-workbench");

  startAttempt("boot");
}

void tick() {
  if (!g_configured) {
    return;
  }
  const uint32_t now = millis();
  const bool up = (WiFi.status() == WL_CONNECTED);

  if (up) {
    if (g_phase != Phase::Up) {
      enterUp();
    }
    return;
  }

  // 未连上。三种情形分别处理：刚从 Up 掉下来、正在尝试、在等退避。
  if (g_phase == Phase::Up) {
    g_phase = Phase::Trying;  // 先落到 Trying，下面统一按「尝试中」计时
    g_attemptStartMs = now;
    ++g_dropCount;
    g_rssi = 0;
    g_ip[0] = '\0';
    Serial.printf("[wifi] link lost (drops=%lu), reconnecting\n", (unsigned long)g_dropCount);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }

  if (g_phase == Phase::Trying) {
    if (now - g_attemptStartMs < cfg::kWifiAttemptTimeoutMs) {
      return;
    }
    ++g_failCount;
    g_backoffMs = nextBackoff(g_failCount);
    g_phase = Phase::Waiting;
    g_waitStartMs = now;
    Serial.printf("[wifi] attempt timeout after %lu ms, next try in %lu ms\n",
                  (unsigned long)cfg::kWifiAttemptTimeoutMs, (unsigned long)g_backoffMs);
    // 断开这次挂着的半连接，否则下一次 begin() 可能被忽略
    WiFi.disconnect(false, false);
    return;
  }

  if (g_phase == Phase::Waiting) {
    if (now - g_waitStartMs < g_backoffMs) {
      return;
    }
    startAttempt("retry");
  }
}

State state() {
  if (!g_configured) {
    return State::NotConfigured;
  }
  return g_phase == Phase::Up ? State::Connected : State::Connecting;
}

const char *stateName() {
  switch (state()) {
    case State::NotConfigured:
      return "not-configured";
    case State::Connected:
      return "connected";
    default:
      return "connecting";
  }
}

const char *ssid() { return WIFI_SSID; }
int rssi() { return g_rssi; }
const char *ipText() { return g_ip; }
uint32_t connectCount() { return g_connectCount; }
uint32_t dropCount() { return g_dropCount; }

void debugDropLink() {
  if (g_phase != Phase::Up) {
    return;
  }
  Serial.println("[wifi] DEBUG forcing link drop");
  // 只断链路，不擦凭据、不关 STA 模式 —— 让下游状态机和真实掉线看到同一件事。
  WiFi.disconnect(false, false);
}

}  // namespace netwifi
