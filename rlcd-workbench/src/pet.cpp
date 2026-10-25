// rlcd-workbench / 宠物（鹅）状态机实现（v0.1.3，docs/adr/0019）
//
// 实现注（两条，都是设计文档里点了名、但实现层必须再确认一次的）：
//
// 1. 「逾期」这个信号本期只覆盖到「待办停更 / 有坏行」。真正的逐条逾期要解析
//    `due` 绝对时间串（todo.h 明确说过它只原样透传、不校验格式），那是待办模块
//    自己的一次改造，不该打包进「画一只鹅」。所以 Signals.todoTrouble 占的是
//    ADR-0019 §5 优先级表里的第一格，但它的语义目前是「待办出事了」而不是
//    「某条待办过期了」—— 串口日志打的是 todo_stale，不要读成 overdue。
//
// 2. 跨天检测只在 60 s 节拍里做，而且**只在时间可信时**做。RTC 掉电后上电会
//    读到出厂残值（2026-10-23 在 HARDWARE.md 记录过这个情形），拿那种时间算
//    天数，屏幕上看和「真的过了很多天」一模一样。宁可这一天不涨。
//
// 日期往会倒退时（时钟被 NTP 校回、或 RTC 残值），只更新日期基准、**不减**
// bondDays —— 天数是记账不是计时，它只在「新的一天真的到了」时前进。

#include "pet.h"

#include <Preferences.h>

#include "clock.h"
#include "config.h"
#include "timeutil.h"

namespace pet {

namespace {

// NVS 键名（namespace 复用 cfg::kSettingsNamespace = "rlcd"）。
// 两个键分开存：bondDay 是「上一次算过的那一天」，bondDays 是累计值。
// 只有前者的话，跨天瞬间断电会丢那一天的增量；只有后者的话，无法判断今天
// 是不是已经算过了 —— 于是每次开机都涨一天。
constexpr const char *kKeyDay = "bondDay";    // 本地 yyyy-mm-dd，整型
constexpr const char *kKeyDays = "bondDays";  // 累计天数

constexpr uint32_t kCheckDayMs = 60000;  // 跨天检测挂的节拍（ADR-0019 决定 4）

// 动画窗口长度。> 3.9 V 与 < 3.9 V 用两个值（ADR-0019 决定 6 / 决定 9）：
// 低电时不是「不播」，是「播短一点」—— 用户仍然能看见「它注意到我了」，
// 而整帧刷的次数被砍掉四成。
constexpr uint32_t kBurstNormalMs = 5000;
constexpr uint32_t kBurstLowBatMs = 3000;

constexpr uint32_t kSeenMs = 1500;  // 抬头（ADR-0019 决定 10：1.5 s）
constexpr uint32_t kGladMs = 3000;  // 待办清零那一下
constexpr uint32_t kDayTurnMs = 5000;  // 跨天当天：围巾/帽子亮相 5 s

constexpr uint32_t kDozeMs = 5u * 60u * 1000u;   // 5 分钟没按键 -> 犯困
constexpr uint32_t kSleepMs = 30u * 60u * 1000u;  // 30 分钟 -> 睡着

constexpr uint16_t kBurstStepMs = 125;  // 8 Hz

// 第 7 天围巾 2026-10-25 取消：黑块 bump 仍把脖子读成「切断」，
// 第 7 天不再换图，里程碑只剩第 30 天帽子。
constexpr uint32_t kHatDay = 30;    // 第 30 天帽子

struct State {
  Mood mood = Mood::Idle;
  Why why = Why::None;
  uint32_t bondDays = 0;
  int32_t lastDayKey = 0;  // yyyymmdd；0 = 还没有
  bool haveDay = false;
  uint32_t bondUntilMs = 0;
  uint32_t seenUntilMs = 0;
  uint32_t gladUntilMs = 0;
  uint32_t burstUntilMs = 0;
  uint32_t lastCheckMs = 0;
  bool lowPower = false;
  bool hot = false;
  int openTodos = -1;
} g;

Preferences g_prefs;
bool g_prefsOpen = false;

// 打开 namespace。失败不致命：天数这个版本就不跨重启，屏上照常有一只鹅。
bool ensureOpen(bool create) {
  if (g_prefsOpen) {
    return true;
  }
  g_prefsOpen = g_prefs.begin(cfg::kSettingsNamespace, create);
  return g_prefsOpen;
}

int32_t dayKeyOf(const tu::DT &t) { return (int32_t)t.year * 10000 + t.month * 100 + t.day; }

void writeDays() {
  if (!ensureOpen(true)) {
    return;
  }
  g_prefs.putUInt(kKeyDay, (uint32_t)g.lastDayKey);
  g_prefs.putUInt(kKeyDays, g.bondDays);
}

// 窗口起点。已在窗口内就取更晚的那个：连续事件不该把窗口缩短。
void startBurst(uint32_t nowMs, uint32_t wantMs) {
  const uint32_t until = nowMs + wantMs;
  if ((int32_t)(until - g.burstUntilMs) > 0) {
    g.burstUntilMs = until;
  }
}

uint32_t burstLengthMs() { return g.lowPower ? kBurstLowBatMs : kBurstNormalMs; }

// 60 s 节拍上的跨天检测。只有「今天是可信日期且和上次算过的那天不同」才动天数。
void checkDay(bool timeReliable, uint32_t nowMs) {
  if (!timeReliable) {
    return;
  }
  const tu::DT &t = workclock::now();
  const int32_t key = dayKeyOf(t);
  if (g.haveDay && key == g.lastDayKey) {
    return;
  }

  if (!g.haveDay) {
    // 第一次登记：从今天开始算「一起的第 1 天」。
    // 这里不看 bondDays 是不是已经有值 —— 只有 bondDay 丢了的极端情况才会
    // 走到「haveDay=false 但 bondDays>0」，那种情况要保住累计值不能抹成 1。
    g.lastDayKey = key;
    g.haveDay = true;
    if (g.bondDays == 0) {
      g.bondDays = 1;
    }
    writeDays();
    return;
  }

  // 上一次的日期拆回年月日，再和今天做日历差（daysFromCivil 跨年跨闰年都成立）。
  const int prevY = (int)(g.lastDayKey / 10000);
  const int prevM = (int)((g.lastDayKey / 100) % 100);
  const int prevD = (int)(g.lastDayKey % 100);
  const int64_t prevDays = tu::daysFromCivil(prevY, (unsigned)prevM, (unsigned)prevD);
  const int64_t todayDays = tu::daysFromCivil(t.year, (unsigned)t.month, (unsigned)t.day);
  const int64_t diff = todayDays - prevDays;

  // 日期基准无论如何先跟上：不然时间倒退之后每一秒都会重新进这个分支。
  g.lastDayKey = key;

  if (diff <= 0) {
    // 时间倒退。不减天数，只把基准挪过去；下一次真的跨天才继续涨。
    writeDays();
    return;
  }

  // 断电五天回来一次涨 5（ADR-0019 §4：记的是「一起过了多少天」）。
  g.bondDays += (uint32_t)diff;
  g.bondUntilMs = nowMs + kDayTurnMs;
  startBurst(nowMs, burstLengthMs());
  writeDays();
}

}  // namespace

void begin() {
  g.bondDays = 0;
  g.lastDayKey = 0;
  g.haveDay = false;
  if (!ensureOpen(false)) {
    return;
  }
  const uint32_t day = g_prefs.getUInt(kKeyDay, 0);
  if (day != 0) {
    g.lastDayKey = (int32_t)day;
    g.haveDay = true;
  }
  g.bondDays = g_prefs.getUInt(kKeyDays, 0);
}

void tick(const Signals &s, uint32_t nowMs) {
  g.lowPower = s.lowPower;
  g.hot = s.hot;

  // ---- 跨天（60 s 节拍）----
  if (g.lastCheckMs == 0 || (uint32_t)(nowMs - g.lastCheckMs) >= kCheckDayMs) {
    g.lastCheckMs = nowMs;
    checkDay(s.timeReliable, nowMs);
  }

  // ---- 「待办清零那一刻」是个事件，不是稳态 ----
  // 从「还有 N 条」变成「0 条」才算；开机就一条不剩不算 —— 那不是「刚做完」，
  // 是「本来就没有」，两种情形在屏上都表现为「没有待办」，但只有前者值得高兴。
  if (s.openTodos >= 0 && g.openTodos > 0 && s.openTodos == 0) {
    g.gladUntilMs = nowMs + kGladMs;
    startBurst(nowMs, burstLengthMs());
  }
  g.openTodos = s.openTodos;

  // ---- 主表情：优先级固定为 逾期/待办异常 > 服务器异常 > 低电 > 久未互动 > idle ----
  // （ADR-0019 §5。冲突时取高者；温湿度只做细节，不参与这里的判断。）
  Why why = Why::None;
  Mood mood = Mood::Idle;
  if (s.todoTrouble) {
    mood = Mood::Unhappy;
    why = Why::TodoTrouble;
  } else if (s.serverDown) {
    mood = Mood::Unhappy;
    why = Why::ServerDown;
  } else if (s.lowPower) {
    mood = Mood::Unhappy;
    why = Why::LowBat;
  } else if (s.idleMs >= kSleepMs) {
    mood = Mood::Sleep;
    why = Why::IdleVeryLong;
  } else if (s.idleMs >= kDozeMs) {
    mood = Mood::Doze;
    why = Why::IdleLong;
  }
  g.mood = mood;
  g.why = why;
}

void seen(uint32_t nowMs) {
  g.seenUntilMs = nowMs + kSeenMs;
  // 抬头只有 1.5 s，窗口跟着它走 —— 用 8 Hz 播一个 1.5 s 的动作，
  // 播完就回 1 Hz，而不是「为了 1.5 s 的动作开 5 s 的窗口」。
  startBurst(nowMs, kSeenMs);
}

bool burstActive(uint32_t nowMs) { return (int32_t)(nowMs - g.burstUntilMs) < 0; }

uint16_t burstStepMs() { return kBurstStepMs; }

uint32_t bondDays() { return g.bondDays; }

View sample(uint32_t nowMs) {
  View v;
  v.bondDays = g.bondDays;
  v.accessory = (g.bondDays >= kHatDay) ? 2 : 0;
  v.breath = (uint8_t)((nowMs / 1000u) % 4u);
  v.pant = g.hot;

  const bool burstWindow = burstActive(nowMs);
  if (burstWindow) {
    // 窗口内的 4 帧步态。窗口外恒为 0：静止的鹅不该有一只脚悬在半空。
    v.stride = (uint8_t)((nowMs / kBurstStepMs) % 4u);
  }

  v.lookingUp = (int32_t)(nowMs - g.seenUntilMs) < 0;
  v.blink = !v.lookingUp && ((nowMs / 1000u) % 5u) == 4u;

  // 事件性表情压过主表情，它们之间也有先后：抬头 > 跨天 > 高兴 > 主表情。
  if (v.lookingUp) {
    v.mood = Mood::Seen;
    v.why = Why::BootShort;
  } else if ((int32_t)(nowMs - g.bondUntilMs) < 0) {
    v.mood = Mood::Bond;
    v.why = Why::DayTurned;
  } else if ((int32_t)(nowMs - g.gladUntilMs) < 0) {
    v.mood = Mood::Glad;
    v.why = Why::Cleared;
  } else {
    v.mood = g.mood;
    v.why = g.why;
  }

  // 帧号：ui 按这个下标到 goose_frames.h 里取点阵，不再自己推断「该画哪张」。
  // 0=idle 1=blink 2=seen 3=glad 4=unhappy 5=sleep 6=walk1 7=walk2 8=hat 9=pant
  // （第 7 天围巾帧 2026-10-25 取消：黑块 bump 仍把脖子读成「切断」，不再换图。）
  if (v.pant) {
    v.frame = 9;
  } else if (v.mood == Mood::Sleep || v.mood == Mood::Doze) {
    v.frame = 5;
  } else if (v.stride == 1) {
    v.frame = 6;
  } else if (v.stride == 3) {
    v.frame = 7;
  } else if (v.mood == Mood::Seen) {
    v.frame = 2;
  } else if (v.mood == Mood::Glad || v.mood == Mood::Bond) {
    v.frame = 3;
  } else if (v.mood == Mood::Unhappy) {
    v.frame = 4;
  } else if (v.blink) {
    v.frame = 1;
  } else if (v.accessory >= 2) {
    v.frame = 8;
  } else {
    v.frame = 0;
  }
  return v;
}

const char *moodName(Mood m) {
  switch (m) {
    case Mood::Idle:    return "idle";
    case Mood::Glad:    return "glad";
    case Mood::Unhappy: return "unhappy";
    case Mood::Doze:    return "doze";
    case Mood::Sleep:   return "sleep";
    case Mood::Seen:    return "seen";
    case Mood::Bond:    return "bond";
    default:            return "?";
  }
}

const char *whyName(Why w) {
  switch (w) {
    case Why::None:         return "none";
    case Why::TodoTrouble:  return "todo_trouble";
    case Why::ServerDown:   return "server_down";
    case Why::LowBat:       return "lowbat";
    case Why::IdleLong:     return "idle_long";
    case Why::IdleVeryLong: return "idle_very_long";
    case Why::Cleared:      return "cleared";
    case Why::DayTurned:    return "day_turned";
    case Why::BootShort:    return "boot_short";
    default:                return "?";
  }
}

}  // namespace pet
