// rlcd-workbench / 主程序（M2：接真实传感器）
//
// M1 的页面骨架与两键六手势已完成。M2 起逐步把 fillFakeData() 里的假数据换成
// 真值，顺序是：I2C 总线与 SHTC3 温湿度 → 电量 → WiFi → 服务器数据 → RTC + NTP。
// 本文件的职责仍是：状态机（面板 / 关于页 / 对比度）、按键分发、刷新与省电策略。

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "clock.h"
#include "battery.h"
#include "config.h"
#include "i2c.h"
#include "input.h"
#include "rlcd.h"
#include "shtc3.h"
#include "snapshot.h"
#include "timesync.h"
#include "timeutil.h"
#include "tz.h"
#include "ui.h"
#include "net_wifi.h"

namespace {

RlcdDisplay g_lcd(cfg::kLcdSck, cfg::kLcdMosi, cfg::kLcdDc, cfg::kLcdCs, cfg::kLcdRst);
AppState g_state;

uint8_t g_contrastIdx = 2;  // 默认第 3 档 = 60
uint32_t g_contrastHintUntil = 0;
uint32_t g_bootHintUntil = 0;
uint32_t g_lastActivityMs = 0;
uint32_t g_lastSecond = 0;
uint32_t g_lastSensorMs = 0;
uint32_t g_lastBatteryMs = 0;
uint32_t g_lastWifiMs = 0;
bool g_lpm = false;
bool g_dirty = true;
bool g_sensorOk = false;
bool g_batteryOk = false;
bool g_dropTestDone = false;
timesync::Source g_lastTimeSource = timesync::Source::None;

void logLine(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// 把工作时钟的时间抄进 UI 状态。
//
// M1 之前这里没有这一步：每秒只写 g_state.second，于是分和时永远停在
// 开机灌进去的 10:22 —— 秒在跳、分钟不动。进位逻辑现在统一在
// workclock / tu::addOneSecond 里，主程序只在秒边界搬运结果。
void syncClockToState() {
  const tu::DT &t = workclock::now();
  g_state.year = t.year;
  g_state.month = t.month;
  g_state.day = t.day;
  g_state.weekday = t.weekday;
  g_state.hour = t.hour;
  g_state.minute = t.minute;
  g_state.second = t.second;
}

// ---- 时间进位自检 ----
//
// 分钟进位在屏幕上一眼能看见，跨天 / 跨月 / 跨年 / 闰年进位看不出来。
// 用户原话「顺带检查一下时间进位的逻辑」——把边界在开机时全跑一遍，
// 结果直接打进串口，不靠肉眼看表。
//
// 星期几的期望值是从 2026-10-22 = 周四倒推的，不是抄的常量。

struct ClockCase {
  const char *name;
  int y, mo, d, wd, h, mi, s;
  int ey, emo, ed, ewd, eh, emi, es;
};

void clockSelfTest() {
  static const ClockCase cases[] = {
      // name                 起始                                    期望
      {"sec->min",       2026, 10, 22, 4, 10, 22, 59,  2026, 10, 22, 4, 10, 23, 0},
      {"min->hour",      2026, 10, 22, 4, 10, 59, 59,  2026, 10, 22, 4, 11,  0, 0},
      {"hour->day",      2026, 10, 22, 4, 23, 59, 59,  2026, 10, 23, 5,  0,  0, 0},
      {"31->next month", 2026, 10, 31, 6, 23, 59, 59,  2026, 11,  1, 7,  0,  0, 0},
      {"30->next month", 2026, 11, 30, 1, 23, 59, 59,  2026, 12,  1, 2,  0,  0, 0},
      {"year roll",      2026, 12, 31, 4, 23, 59, 59,  2027,  1,  1, 5,  0,  0, 0},
      {"leap 2/28",      2028,  2, 28, 1, 23, 59, 59,  2028,  2, 29, 2,  0,  0, 0},
      {"nonleap 2/28",   2026,  2, 28, 6, 23, 59, 59,  2026,  3,  1, 7,  0,  0, 0},
  };

  int fails = 0;
  for (const ClockCase &c : cases) {
    tu::DT t;
    t.year = c.y;
    t.month = c.mo;
    t.day = c.d;
    t.weekday = c.wd;
    t.hour = c.h;
    t.minute = c.mi;
    t.second = c.s;

    tu::addOneSecond(t);

    const bool ok = (t.year == c.ey && t.month == c.emo && t.day == c.ed && t.weekday == c.ewd &&
                     t.hour == c.eh && t.minute == c.emi && t.second == c.es);
    if (!ok) ++fails;
    logLine("[clk-test] +1s  %-15s %s", c.name, ok ? "OK" : "FAIL");
  }

  // 月历那一侧：addDays 跨月跨年（退位）
  int wy = 2026, wm = 1, wd = 3;
  tu::addDays(wy, wm, wd, -7);
  const bool okBack = (wy == 2025 && wm == 12 && wd == 27);
  if (!okBack) ++fails;
  logLine("[clk-test] -7d  %-15s %s", "addDays back", okBack ? "OK" : "FAIL");

  logLine("[clk-test] result = %s (%d fail)", fails == 0 ? "OK" : "FAIL", fails);
}

// ---- 温湿度 ----
//
// 一次读数里有三个层次，日志全部打出来，因为「哪个才是对的」取决于校准：
//   1. rawTemp / rawHumi：寄存器原始 16 位，两段 CRC 都已校验；
//   2. tempC  ：数据手册公式 175*raw/65536 - 45；
//   3. tempAdjC：再叠加官方例程那个 -4 °C 补偿。
// 官方为什么减 4 我没有权威依据（i2c_equipment.cpp:98 的 SHTC3_PETP_VOL = 4），
// 所以两个值都印出来 —— 拿手持表一对就知道该采用哪个（§7 第 9 项）。
void sensorSelfTest() {
  if (!shtc3::ready()) {
    logLine("[shtc3] self-test skipped: sensor not ready");
    return;
  }
  logLine("[shtc3] id = 0x%04X", (unsigned)shtc3::sensorId());

  for (int i = 0; i < 3; ++i) {
    shtc3::Reading r;
    if (!shtc3::read(&r)) {
      logLine("[shtc3] sample %d: FAIL (i2c err=%d)", i + 1, i2cbus::lastError());
      continue;
    }
    logLine("[shtc3] sample %d: raw=%u/%u  t=%.2fC  t_adj=%.2fC  rh=%.1f%%", i + 1,
            (unsigned)r.rawTemp, (unsigned)r.rawHumi, r.tempC, r.tempAdjC, r.humi);
    delay(120);
  }

  // 字节序判定（§7 第 9 项）。
  //
  // 换用「湿度优先」的命令 0x58E0 再量一次，把芯片返回的两对原始值原样打出来
  // （不做任何解释）。如果 0x7866 是「先温度」、0x58E0 是「先湿度」，两组
  // 「第一对」就应该互换 —— 那说明命令字有效、解码没错，读数与体感不符只能
  // 是别的物理原因。两组一模一样则说明命令字被忽略、或根本不是这颗芯片。
  shtc3::Reading rh;
  if (shtc3::readRhFirst(&rh)) {
    logLine("[shtc3] cmd 0x58E0 (RH first): first=%u second=%u", (unsigned)rh.rawFirst,
            (unsigned)rh.rawSecond);
  } else {
    logLine("[shtc3] cmd 0x58E0: FAIL (i2c err=%d)", i2cbus::lastError());
  }

  // 芯片内部温度只作参照：它读的是 SoC 结温，恒定高于环境温度，
  // 但能回答「板子到底热不热」这个问题。
  logLine("[shtc3] ref: chip internal = %.1fC (not ambient)", (double)temperatureRead());
}

// 时区表自检：用几个固定的 UTC 时刻去问「这个时区当时偏移多少」。
//
// 为什么要开机跑一遍：DST 规则一旦写错，只有到换季那天才会暴露 —— 而那时
// 谁也想不到是这里的问题。这里把南北半球、跨年 DST、无 DST 三类都钉住。
// 检查完会把时区恢复成 cfg::kTimeZoneName，不影响后续逻辑。
void timeSelfTest() {
  struct Case {
    const char *zone;
    int64_t utc;
    int expectMin;
  };

  // 2026-07-15 12:00 UTC（北半球夏令时中 / 南半球冬令时中）
  const tu::DT jul = [] {
    tu::DT t;
    t.year = 2026;
    t.month = 7;
    t.day = 15;
    t.hour = 12;
    t.minute = 0;
    t.second = 0;
    return t;
  }();
  // 2026-01-15 12:00 UTC（反过来）
  tu::DT jan = jul;
  jan.month = 1;

  const int64_t julUtc = tu::toEpoch(jul);
  const int64_t janUtc = tu::toEpoch(jan);

  const Case cases[] = {
      {"UTC", julUtc, 0},
      {"Asia/Shanghai", julUtc, 480},
      {"Asia/Tokyo", janUtc, 540},
      {"Asia/Kolkata", julUtc, 330},
      {"Europe/London", janUtc, 0},
      {"Europe/London", julUtc, 60},
      {"Europe/Paris", janUtc, 60},
      {"Europe/Paris", julUtc, 120},
      {"America/New_York", janUtc, -300},
      {"America/New_York", julUtc, -240},
      {"America/Los_Angeles", julUtc, -420},
      {"America/Phoenix", julUtc, -420},  // 亚利桑那不实行 DST，全天候 -7
      {"Australia/Sydney", janUtc, 660},  // 南半球：一月在夏令时里
      {"Australia/Sydney", julUtc, 600},
      {"Pacific/Auckland", janUtc, 780},
      {"Pacific/Auckland", julUtc, 720},
  };

  int fail = 0;
  for (const Case &c : cases) {
    tz::setByName(c.zone);
    const int got = tz::offsetMinutesAt(c.utc);
    const bool ok = (got == c.expectMin);
    if (!ok) ++fail;
    logLine("[tz-test] %-20s %s  got=%+d min expect=%+d min", c.zone, ok ? "OK  " : "FAIL",
            got, c.expectMin);
  }

  // 往返：本地 DT -> UTC -> 本地 DT 必须回到原值
  tz::setByName(cfg::kTimeZoneName);
  tu::DT probe = jul;
  probe.month = 10;
  probe.day = 23;
  probe.hour = 14;
  probe.minute = 13;
  probe.second = 36;
  const tu::DT back = tz::toLocal(tz::toUtc(probe));
  const bool rtOk = back.year == probe.year && back.month == probe.month &&
                    back.day == probe.day && back.hour == probe.hour &&
                    back.minute == probe.minute && back.second == probe.second;
  if (!rtOk) ++fail;
  logLine("[tz-test] roundtrip %s  %04d-%02d-%02d %02d:%02d:%02d -> %04d-%02d-%02d %02d:%02d:%02d",
          rtOk ? "OK  " : "FAIL", probe.year, probe.month, probe.day, probe.hour, probe.minute,
          probe.second, back.year, back.month, back.day, back.hour, back.minute, back.second);

  // epoch <-> 日历 往返 + 星期（2026-10-23 是周五 = 5）
  tu::DT wd = probe;
  wd.weekday = 0;
  const tu::DT wdBack = tu::fromEpoch(tu::toEpoch(wd));
  const bool wdOk = (wdBack.weekday == 5) && (wdBack.year == wd.year) &&
                    (wdBack.month == wd.month) && (wdBack.day == wd.day);
  if (!wdOk) ++fail;
  logLine("[tz-test] weekday  %s  2026-10-23 -> wd=%d (expect 5 = Fri)", wdOk ? "OK  " : "FAIL",
          wdBack.weekday);

  logLine("[tz-test] result = %s (%d fail)", fail == 0 ? "OK" : "FAIL", fail);
}

// 采一次温湿度并写进界面状态。
//
// 失败时保留上一次的值，只在串口留痕 —— 屏幕上突然冒出「--.-」比显示一个
// 几十秒前的读数更糟，而这块屏是常驻桌面、要能一眼扫过去的。
bool readSensors(bool verbose) {
  if (!shtc3::ready()) {
    return false;
  }
  shtc3::Reading r;
  if (!shtc3::read(&r)) {
    if (verbose) {
      logLine("[shtc3] read failed (i2c err=%d)", i2cbus::lastError());
    }
    return false;
  }
  g_state.tempC = r.tempAdjC;
  g_state.humi = r.humi;
  g_sensorOk = true;
  g_dirty = true;
  if (verbose) {
    logLine("[shtc3] t=%.2fC rh=%.1f%% (raw %u/%u)", r.tempAdjC, r.humi, (unsigned)r.rawTemp,
            (unsigned)r.rawHumi);
  }
  return true;
}

// 采一次电池电压并写进界面状态。
//
// 与温湿度同样的取舍：读失败就保持上一次的百分比，不把图标打成空格 ——
// 「电量未知」和「电量为 0」在屏幕上长得一样，但那两件事差得远。
bool readBattery(bool verbose) {
  if (!battery::ready()) {
    return false;
  }
  battery::Sample s;
  if (!battery::read(&s)) {
    if (verbose) {
      logLine("[bat] read failed");
    }
    return false;
  }
  // 只有百分比真的变了才置脏。
  //
  // 电压在几毫伏内晃是常态，而「置脏」在面板二上意味着从 LPM 醒来重画一帧
  // （refreshAll 会先切 HPM 再等 100 ms），为一次没变过的读数付这个代价不值得。
  const int prevPct = g_state.batteryPct;
  g_state.batteryPct = s.pct;
  g_batteryOk = true;
  if (prevPct != s.pct) {
    g_dirty = true;
  }
  if (verbose) {
    logLine("[bat] raw=%d pin=%dmV pack=%.3fV -> %d%%", s.raw, s.pinMv, (double)s.packV, s.pct);
  }
  return true;
}

// 把 netwifi 的三态投影到界面状态；变了才置脏。
//
// 映射刻意只有一对三：Connected -> 满格、Connecting -> 半格、NotConfigured -> 叉。
// 没有「断开」这个独立显示态 —— 只要还在重连，屏幕上就该是半格。
bool syncWifiToState() {
  WifiState want = WIFI_CONNECTING;
  switch (netwifi::state()) {
    case netwifi::State::Connected:
      want = WIFI_ON;
      break;
    case netwifi::State::NotConfigured:
      want = WIFI_OFF;
      break;
    default:
      want = WIFI_CONNECTING;
      break;
  }
  if (g_state.wifi == want) {
    return false;
  }
  g_state.wifi = want;
  g_dirty = true;
  return true;
}

// 把快照投影到界面状态。返回是否变了（变了才置脏）。
//
// 变了的判定不能拿指针比，也不能每次都置脏 —— 面板二从 LPM 醒来重画一帧要
// 先切 HPM 再等 100 ms，为了一个没变的数字付这个代价不值。
bool syncServerToState() {
  const snapshot::Data &d = snapshot::data();
  bool changed = false;

  // 新鲜度先算：即使数值没变，龄期跨过一个整秒也可能要重画页尾。
  const bool hasData = d.valid;
  const uint32_t ageSec = hasData ? snapshot::ageMs() / 1000 : 0;
  const bool staleNow = snapshot::stale();
  if (g_state.serverHasData != hasData || g_state.serverStale != staleNow ||
      g_state.serverAgeSec != ageSec) {
    g_state.serverHasData = hasData;
    g_state.serverStale = staleNow;
    g_state.serverAgeSec = ageSec;
    changed = true;
  }

  if (!hasData) {
    return changed;
  }

  uint8_t mask = 0;
  if (d.hasCpu) {
    mask |= AppState::KN_CPU;
    if (g_state.cpuPct != d.cpuPct) {
      g_state.cpuPct = d.cpuPct;
      changed = true;
    }
  }
  if (d.hasMem) {
    mask |= AppState::KN_MEM;
    if (g_state.memPct != d.memPct || strcmp(g_state.memDetail, d.memDetail) != 0) {
      g_state.memPct = d.memPct;
      snprintf(g_state.memDetail, sizeof(g_state.memDetail), "%s", d.memDetail);
      changed = true;
    }
  }
  if (d.hasDisk) {
    mask |= AppState::KN_DISK;
    if (g_state.diskPct != d.diskPct || strcmp(g_state.diskDetail, d.diskDetail) != 0) {
      g_state.diskPct = d.diskPct;
      snprintf(g_state.diskDetail, sizeof(g_state.diskDetail), "%s", d.diskDetail);
      changed = true;
    }
  }
  if (d.hasLoad) {
    mask |= AppState::KN_LOAD;
    if (g_state.load1 != d.load1) {
      g_state.load1 = d.load1;
      changed = true;
    }
  }
  if (d.hasCert) {
    mask |= AppState::KN_CERT;
    if (g_state.certDays != d.certDays) {
      g_state.certDays = d.certDays;
      changed = true;
    }
  }
  if (g_state.known != mask) {
    g_state.known = mask;
    changed = true;
  }

  const snapshot::Probe probes[6] = {d.svcWeb,  d.svcSelect, d.svcWb,
                                    d.svcApi,  d.svcUpload, d.svcMa12};
  for (int i = 0; i < 6; ++i) {
    ProbeState want = PROBE_UNKNOWN;
    if (probes[i] == snapshot::Probe::Ok) {
      want = PROBE_OK;
    } else if (probes[i] == snapshot::Probe::Fail) {
      want = PROBE_FAIL;
    }
    if (g_state.probes[i].state != want) {
      g_state.probes[i].state = want;
      changed = true;
    }
  }

  return changed;
}

// 自发热诊断：停掉一切重绘，静置 kThermalProbeMs，定期记录两种温度。
//
// 为什么要做这件事：SHTC3 读到 35 °C，而 SoC 结温 36 °C 也跟着走 —— 两者贴得
// 太近，无法判断「这块板本来就 35 °C」还是「我们自己刷屏把它烘热了」。重绘
// 只在面板一发生，1 秒一帧、每帧约 15 ms，理论上只有 1.5% 占空比，但不该靠
// 推测，静置一百八十秒看温度走哪去，比讨论有用。
void runThermalProbe() {
  logLine("[therm] probe start: %lus, no redraw, cpu=%uMHz",
          (unsigned long)(cfg::kThermalProbeMs / 1000), (unsigned)getCpuFrequencyMhz());
  const uint32_t t0 = millis();
  uint32_t next = t0 + cfg::kThermalProbePeriodMs;
  unsigned n = 0;
  while (millis() - t0 < cfg::kThermalProbeMs) {
    if ((int32_t)(millis() - next) >= 0) {
      next += cfg::kThermalProbePeriodMs;
      shtc3::Reading r;
      const bool ok = shtc3::read(&r);
      logLine("[therm] t+%3lus chip=%5.1fC shtc3=%s%.2fC rh=%.1f%%",
              (unsigned long)((millis() - t0) / 1000), (double)temperatureRead(),
              ok ? "" : "FAIL ", ok ? r.tempC : 0.0f, ok ? r.humi : 0.0f);
      ++n;
    }
    delay(50);
  }
  logLine("[therm] probe done, %u samples", n);
}

void fillFakeData() {
  g_state.panel = 0;
  g_state.year = 2026;
  g_state.month = 10;
  g_state.day = 22;
  g_state.weekday = 4;  // 周四
  g_state.hour = 10;
  g_state.minute = 22;
  g_state.second = 31;

  // 以这份假数据为基准启动工作时钟。M2 换成 RTC 读回的真实时间即可，
  // 进位规则与刷新节奏都不用动。
  tu::DT t;
  t.year = g_state.year;
  t.month = g_state.month;
  t.day = g_state.day;
  t.weekday = g_state.weekday;
  t.hour = g_state.hour;
  t.minute = g_state.minute;
  t.second = g_state.second;
  workclock::begin(t);
  g_state.tempC = 24.5f;
  g_state.humi = 48.0f;
  // 温湿度先给一组占位值，shtc3 第一次读数成功就被覆盖。
  // 传感器读不到时屏幕上就是这组数，串口会有 [shtc3] MISSING 或 read failed。
  g_state.batteryPct = 87;
  g_state.wifi = WIFI_CONNECTING;
  g_state.todoTotal = 6;
  g_state.todoDone = 0;

  const char *texts[cfg::kTodoSlots] = {"交周报", "买牛奶", "缴水电费", "换滤芯", "取快递", "整理照片"};
  const char *dues[cfg::kTodoSlots] = {"2h", "明天", "逾期", "3天", "今天", "10-28"};
  for (int i = 0; i < cfg::kTodoSlots; ++i) {
    snprintf(g_state.todos[i].text, sizeof(g_state.todos[i].text), "%s", texts[i]);
    snprintf(g_state.todos[i].due, sizeof(g_state.todos[i].due), "%s", dues[i]);
  }

  g_state.tripMonthKey = 202610;
  memset(g_state.tripCount, 0, sizeof(g_state.tripCount));
  g_state.tripCount[12] = 1;
  g_state.tripCount[22] = 2;
  g_state.tripCount[24] = 2;
  g_state.tripCount[27] = 3;
  g_state.tripCount[30] = 1;

  // 面板二在拿到第一份快照前是「还不知道」，不是「全是 0」也不是「全是叉」。
  // 所以这里只摆好六行的标签，状态统一 Unknown，known 位一个都不置。
  g_state.probeCount = 6;
  g_state.probes[0] = {"www.siungo.top", PROBE_UNKNOWN};
  g_state.probes[1] = {"select.siungo.top", PROBE_UNKNOWN};
  g_state.probes[2] = {"wb.siungo.top", PROBE_UNKNOWN};
  g_state.probes[3] = {"api/photos", PROBE_UNKNOWN};
  g_state.probes[4] = {"api/health", PROBE_UNKNOWN};
  g_state.probes[5] = {"<tcp-host>:<port>", PROBE_UNKNOWN};
  g_state.known = 0;
  g_state.serverHasData = false;
  g_state.serverStale = false;
  g_state.serverAgeSec = 0;
}

// 需要省电时先切 HPM 再写，写完按空闲计时切回 LPM。
void paint() {
  U8G2 *g = g_lcd.g();
  if (g == nullptr) {
    return;
  }
  const bool showHeaderTime = (g_state.panel != 0);  // 面板一隐藏页首时间（ADR-0002）
  if (g_state.about) {
    ui::drawAbout(g, g_state);
  } else {
    ui::drawFrame(g, g_state, showHeaderTime);
  }
  if (g_bootHintUntil != 0 && (int32_t)(millis() - g_bootHintUntil) < 0) {
    ui::drawToast(g, "预留");
  }
  if (g_contrastHintUntil != 0 && (int32_t)(millis() - g_contrastHintUntil) < 0) {
    ui::drawContrastHint(g, g_contrastIdx, cfg::kContrastCount);
  }
  g_lcd.refreshAll();
  g_dirty = false;
}

// 「活动」只由按键产生。重绘本身不算活动，否则秒针每秒重绘一次，
// 空闲计时永远回不到 0，就再也进不了 LPM 了。
void noteActivity() { g_lastActivityMs = millis(); }

void cycleContrast() {
  g_contrastIdx = (g_contrastIdx + 1) % cfg::kContrastCount;
  g_lcd.setContrast(cfg::kContrastLevels[g_contrastIdx]);
  g_contrastHintUntil = millis() + cfg::kContrastHintMs;
  logLine("[key] contrast -> level %u/%u (%u)", (unsigned)(g_contrastIdx + 1),
          (unsigned)cfg::kContrastCount, (unsigned)cfg::kContrastLevels[g_contrastIdx]);
  g_dirty = true;
}

void goHome() {
  g_state.about = false;
  g_state.panel = 0;
  logLine("[key] go home (panel=0, about closed)");
  g_dirty = true;
}

void handleKey(Gesture gs) {
  if (gs == Gesture::None) {
    return;
  }
  noteActivity();
  switch (gs) {
    case Gesture::Short:
      if (g_state.about) {
        g_state.about = false;
        logLine("[key] close about");
      } else {
        g_state.panel = (g_state.panel + 1) % 2;
        logLine("[key] panel -> %d", g_state.panel);
      }
      break;
    case Gesture::Long:
      goHome();
      break;
    case Gesture::Double:
      g_state.about = !g_state.about;
      logLine("[key] about -> %d", (int)g_state.about);
      break;
    default:
      break;
  }
  g_dirty = true;
}

void handleBoot(Gesture gs) {
  if (gs == Gesture::None) {
    return;
  }
  noteActivity();
  switch (gs) {
    case Gesture::Short:
      logLine("[boot] refresh data + repaint");
      g_dirty = true;
      break;
    case Gesture::Long:
      cycleContrast();
      break;
    case Gesture::Double:
      g_bootHintUntil = millis() + cfg::kContrastHintMs;
      logLine("[boot] reserved slot, not bound");
      g_dirty = true;
      break;
    default:
      break;
  }
}

void printBootReport() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("project : rlcd-workbench");
  Serial.println("stage   : M2 boot (i2c + shtc3 + battery + wifi)");
  Serial.println("----------------------------------------");
  Serial.printf("chip    : %s rev %d\n", ESP.getChipModel(), (int)ESP.getChipRevision());
  Serial.printf("cores   : %d @ %d MHz\n", (int)ESP.getChipCores(), (int)getCpuFrequencyMhz());
  Serial.printf("sdk     : %s\n", ESP.getSdkVersion());
  Serial.printf("flash   : %lu KB total\n", (unsigned long)(ESP.getFlashChipSize() / 1024));
  Serial.printf("heap    : %lu KB free / %lu KB total\n",
                (unsigned long)(ESP.getFreeHeap() / 1024),
                (unsigned long)(ESP.getHeapSize() / 1024));
  Serial.printf("psram   : %lu bytes\n", (unsigned long)ESP.getPsramSize());
  U8G2 *g = g_lcd.g();
  Serial.printf("canvas  : %ux%u\n", g ? (unsigned)g->getDisplayWidth() : 0U,
                g ? (unsigned)g->getDisplayHeight() : 0U);
  Serial.printf("i2c     : sda=%d scl=%d @%lu Hz, %s\n", cfg::kI2cSda, cfg::kI2cScl,
                (unsigned long)cfg::kI2cClockHz, i2cbus::ready() ? "up" : "DOWN");
  Serial.printf("shtc3   : %s, id=0x%04X\n", shtc3::ready() ? "ready" : "MISSING",
                (unsigned)shtc3::sensorId());
  Serial.printf("battery : %s, %d%%\n", battery::ready() ? "ready" : "MISSING",
                g_state.batteryPct);
  Serial.printf("wifi    : %s, ssid=%s\n", netwifi::stateName(), netwifi::ssid());
  Serial.printf("time    : source=%s, rtc=%s, synced=%s\n", timesync::sourceName(),
                timesync::rtcPresent() ? "present" : "MISSING",
                timesync::synced() ? "yes" : "no");
  Serial.printf("clock   : %04d-%02d-%02d %02d:%02d:%02d wd=%d\n", g_state.year, g_state.month,
                g_state.day, g_state.hour, g_state.minute, g_state.second, g_state.weekday);
  Serial.printf("contrast: level %u/%u (%u)\n", (unsigned)(g_contrastIdx + 1),
                (unsigned)cfg::kContrastCount, (unsigned)cfg::kContrastLevels[g_contrastIdx]);
  Serial.println("========================================");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  input::begin();
  input::suppressBootFor(cfg::kBootKeyIgnoreMs);  // 上电 1 秒内丢弃 BOOT

  if (!g_lcd.begin()) {
    Serial.println("[fatal] display init failed");
    while (true) {
      delay(1000);
    }
  }
  g_lcd.setContrast(cfg::kContrastLevels[g_contrastIdx]);

  fillFakeData();
  clockSelfTest();

  // ---- M2 第 1+2 步：I2C 总线 + SHTC3 ----
  // 总线先建，扫描一次把整条线上的从设备列出来（这块板上应有 0x18 / 0x40 / 0x51 / 0x70）。
  // 失败也不至于让设备变砖：屏幕不依赖 I2C，最坏情况是温湿度那几个字停在占位值。
  if (i2cbus::begin(cfg::kI2cSda, cfg::kI2cScl)) {
    logLine("[i2c] bus up (sda=%d scl=%d)", cfg::kI2cSda, cfg::kI2cScl);
    i2cbus::scan();
  } else {
    logLine("[i2c] bus init FAILED (err=%d)", i2cbus::lastError());
  }

  if (shtc3::begin()) {
    sensorSelfTest();
  } else {
    logLine("[shtc3] init FAILED (err=%d)", i2cbus::lastError());
  }
  readSensors(false);
  g_lastSensorMs = millis();

  // ---- M2 第 3 步：电量 ADC ----
  // 走 IDF 的 adc_oneshot + 曲线拟合校准，不用 analogRead()：后者拿不到校准值，
  // 毫伏数系统偏低的量足以把「满电」显示成「八成」。
  if (battery::begin()) {
    readBattery(false);
    // 连读三次打出来：这块板的分压点并了两颗 100 nF，正常应该稳定在几毫伏内。
    // 要是三次差几十毫伏，说明是开关电源纹波，界面上得再加迟滞。
    for (int i = 0; i < 3; ++i) {
      readBattery(true);
      delay(50);
    }
  } else {
    logLine("[bat] init FAILED");
  }
  g_lastBatteryMs = millis();

  // ---- M2 第 4 步：WiFi ----
  // begin() 只发起第一次连接，不等结果 —— 连上要好几秒，不能在这里阻塞。
  netwifi::begin();
  g_lastWifiMs = millis();
  syncWifiToState();

  // ---- M2 第 5 步：时钟链路 ----
  // begin() 只读 RTC，不发 NTP —— 联网校时在 loop 里由 timesync::tick() 异步推进。
  // 它会把 workclock 覆盖成 RTC 里的真实时间；RTC 不可信时保持 fillFakeData 的占位，
  // 屏上照常走秒，但 timesync::synced() 是 false，boot report 里能看出来。
  timeSelfTest();  // 先验表，再读表 —— 表错的时候后面的结果都不值得看
  timesync::begin();
  g_lastTimeSource = timesync::source();
  syncClockToState();

  // ---- M2 第 6 步：服务器快照 ----
  // 只挂上，不在 setup 里阻塞等 —— 第一份数据在 loop 里由 tick() 拉，
  // 拉回来之前面板二显示的是「还不知道」（空心问号），不是「全挂了」。
  snapshot::begin();
  syncServerToState();

  printBootReport();

  const uint32_t t0 = millis();
  paint();
  Serial.printf("[m1] first full frame = %u ms\n", (unsigned)(millis() - t0));
  noteActivity();
  g_lastSecond = millis() / 1000;

  if (cfg::kThermalProbe) {
    runThermalProbe();  // 诊断完把 cfg::kThermalProbe 改回 false
    noteActivity();
    g_lastSecond = millis() / 1000;
  }
}

void loop() {
  // 按键扫描必须 5 ms 一次，所以主循环不阻塞
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 5) {
    lastTick = millis();
    input::tick();
  }

  handleKey(input::drain(Key::User));
  handleBoot(input::drain(Key::Boot));

  // 秒变化：整帧重绘。
  //
  // 原本想用 refreshArea 只刷左栏那一小块，但实测秒针根本不动 ——
  // u8g2_UpdateDisplayArea 收的是「面板原生竖屏」的 tile 坐标，而整块画布是
  // 按 U8G2_R1 转置过再摆的，逻辑坐标直接换算会落到别的竖带上去，
  // 于是缓冲区里秒变了、真正送显的还是老区域。修它要把 R1 的像素映射做对，
  // 单独一轮做（§7 待验证第 5 项）。在那之前先整帧重绘：9.5 ms，一秒一次。
  //
  // LPM 下不刷：面板自刷新已降到 1 Hz，再写显存只会更糊，不如冻结画面，
  // 任意按键唤醒时整帧重画一次补上。
  const uint32_t sec = millis() / 1000;
  if (sec != g_lastSecond) {
    // 跨过去的秒要补齐。主循环被长任务（WiFi/HTTP/整帧重绘）阻塞几秒时，
    // 只 +1 会让整块表慢慢走慢；delta 用无符号减法，millis 回绕也安全。
    const uint32_t delta = sec - g_lastSecond;
    g_lastSecond = sec;

    const int prevMinute = workclock::now().minute;
    workclock::advance(delta);
    syncClockToState();

    // 每分钟一行日志：验「分有没有进位」不用盯着屏幕等一分钟。
    if (workclock::now().minute != prevMinute) {
      const tu::DT &t = workclock::now();
      logLine("[clock] 20%02d-%02d-%02d %02d:%02d:%02d", t.year % 100, t.month, t.day, t.hour,
              t.minute, t.second);
    }

    if (!g_state.about && g_state.panel == 0 && !g_lpm) {
      g_dirty = true;
    }
  }

  // 对比度角标 / 预留提示到期后要擦掉
  if (g_contrastHintUntil != 0 && (int32_t)(millis() - g_contrastHintUntil) >= 0) {
    g_contrastHintUntil = 0;
    g_dirty = true;
  }
  if (g_bootHintUntil != 0 && (int32_t)(millis() - g_bootHintUntil) >= 0) {
    g_bootHintUntil = 0;
    g_dirty = true;
  }

  // 温湿度 60 秒一次。单次采样在 I2C 上约 70 ms（唤醒 50 + 测量 20），
  // 这个时长会让秒边界掉一拍 —— 所以下面用 delta 补齐而不是固定 +1。
  if (millis() - g_lastSensorMs >= cfg::kSensorPollMs) {
    g_lastSensorMs = millis();
    readSensors(true);
  }

  // 电量 60 秒一次。只打日志，不无条件置脏 —— 电压在几毫伏内晃是常态，
  // 每分钟为它重画一帧不值当；百分比真变了 readBattery() 自己会置脏。
  if (millis() - g_lastBatteryMs >= cfg::kBatteryPollMs) {
    g_lastBatteryMs = millis();
    readBattery(true);
  }

  // WiFi 状态机 100 ms 走一格。它内部有 15 s 尝试超时与 5/10/20/40/60 s 退避，
  // 调得太稀会让退避计时失真；调得太密则是无意义地查 WiFi.status()。
  if (millis() - g_lastWifiMs >= cfg::kWifiTickMs) {
    g_lastWifiMs = millis();
    netwifi::tick();
    syncWifiToState();  // 变了才置脏
  }

  // bringup 开关：验证重连链用，正常固件 kWifiDropTestMs = 0 永不触发。
  if (cfg::kWifiDropTestMs > 0 && !g_dropTestDone && millis() >= cfg::kWifiDropTestMs) {
    g_dropTestDone = true;
    netwifi::debugDropLink();
  }

  // 时间源调度：非阻塞。联网后自动发起 SNTP，成功即写回 RTC；
  // 之后每 24 小时重校一次（RTC 的日累积误差要靠它压住）。
  timesync::tick();
  if (timesync::source() != g_lastTimeSource) {
    g_lastTimeSource = timesync::source();
    syncClockToState();
    g_dirty = true;
  }

  // 服务器快照：非阻塞，联网后每 15 秒拉一次。失败不清旧数据 ——
  // 屏上宁可显示「42 秒前的读数 + 停更标记」，也不要一片空白。
  snapshot::tick();
  if (syncServerToState()) {
    g_dirty = true;
  }

  if (g_dirty) {
    if (g_lpm) {
      g_lpm = false;
      logLine("[pwr] wake to HPM");
    }
    paint();
  }

  // 空闲进 LPM：全反射屏不熄屏，只把面板自刷新降到低频。
  //
  // 但**面板一不进 LPM**：它每秒都要跳一次秒针，写显存前必须先切回 HPM
  // 再等 100 ms（见 RlcdDisplay::refreshAll），也就是说进 LPM 之后每秒都得
  // 来回切一次 —— 不但省不到电，还白搭 100 ms 和一次模式切换。
  // 更早的版本没区分这一条，结果开机 30 秒后进 LPM，秒针就此冻住再也不动。
  const bool clockVisible = (!g_state.about && g_state.panel == 0);
  if (!g_lpm && !clockVisible && (millis() - g_lastActivityMs) > cfg::kIdleToLpmMs) {
    g_lcd.setLpm();
    g_lpm = true;
    logLine("[pwr] idle %u s -> LPM (0x39)", (unsigned)(cfg::kIdleToLpmMs / 1000));
  }
}
