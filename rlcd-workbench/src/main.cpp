// rlcd-workbench / 主程序（M1：页面骨架 + 两键六手势）
//
// M1 阶段数据全是假的（fillFakeData），M2 才接 RTC / 传感器 / 服务器。
// 本文件的职责是：状态机（面板 / 关于页 / 对比度）、按键分发、刷新与省电策略。

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "clock.h"
#include "config.h"
#include "input.h"
#include "rlcd.h"
#include "ui.h"

namespace {

RlcdDisplay g_lcd(cfg::kLcdSck, cfg::kLcdMosi, cfg::kLcdDc, cfg::kLcdCs, cfg::kLcdRst);
AppState g_state;

uint8_t g_contrastIdx = 2;  // 默认第 3 档 = 60
uint32_t g_contrastHintUntil = 0;
uint32_t g_bootHintUntil = 0;
uint32_t g_lastActivityMs = 0;
uint32_t g_lastSecond = 0;
bool g_lpm = false;
bool g_dirty = true;

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
  g_state.batteryPct = 87;
  g_state.wifi = WIFI_ON;
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

  g_state.probeCount = 4;
  g_state.probes[0] = {"www.siungo.top", true};
  g_state.probes[1] = {"select.siungo.top", true};
  g_state.probes[2] = {"api/photos", false};
  g_state.probes[3] = {"api/health", true};
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
  Serial.println("stage   : M1 boot");
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
  printBootReport();

  const uint32_t t0 = millis();
  paint();
  Serial.printf("[m1] first full frame = %u ms\n", (unsigned)(millis() - t0));
  noteActivity();
  g_lastSecond = millis() / 1000;
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
