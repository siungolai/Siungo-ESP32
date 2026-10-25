// rlcd-workbench / 宠物（鹅）状态机（v0.1.3，docs/adr/0019）
//
// 这一层只做三件事，而且三件都是「别人状态的翻译」—— 鹅没有自己的待办：
//   1. 表情：把「待办异常 / 服务器异常 / 低电 / 久未互动 / idle」翻成一只鹅的姿态
//   2. 长期状态：bondDays（本地日历日、只增不减、NVS 一天一写）
//   3. 动画窗口：idle 1 Hz（蹭主循环每秒那次整帧刷）、事件后 3–5 s 的 8 Hz 窗口
//
// 边界（照 settings.cpp 的写法立的）：
//   - 本文件不认识 u8g2，也不画任何东西。姿态在 ui.cpp 的 drawGoose() 里，
//     这里只给出「这一帧应该是什么表情」。
//   - 本文件不打日志。串口那一行由 main.cpp 观察窗口起止自己打 —— 谁持有
//     logLine() 谁负责说，避免两个模块各说一遍同一次切换。
//   - 「日期可不可信」由 main.cpp 判（timesync 归它管），只把结论传进来。
//     不传的话，RTC 掉电读回出厂残值时天数会乱跳，而那种跳变在屏幕上
//     看起来和「真的过了很多天」一模一样。

#pragma once

#include <stdint.h>

namespace pet {

// 表情。这不是「鹅的心情」—— 每个值都对应一个屏上别处已经存在的状态：
//   Unhappy 后面跟着 Why，用来区分到底是待办、服务器还是电量。
enum class Mood : uint8_t {
  Idle = 0,   // 呼吸 + 偶发眨眼
  Glad,       // 待办从「还有」变成「一条不剩」那一刻（3 s）
  Unhappy,    // 待办异常 / 服务器异常 / 低电
  Doze,       // 5 分钟没有任何按键
  Sleep,      // 30 分钟没有任何按键
  Seen,       // BOOT 短按拉数据：抬头 1.5 s
  Bond,       // 跨天当天
};

// 「为什么不高兴」/「这算什么事件」。串口只打 ASCII 名字，屏上不显示。
enum class Why : uint8_t {
  None = 0,
  TodoTrouble,
  ServerDown,
  LowBat,
  IdleLong,
  IdleVeryLong,
  Cleared,    // 待办清零（Glad 的成因）
  DayTurned,  // 跨天（Bond 的成因）
  BootShort,  // BOOT 短按拉数据（Seen 的成因）
};

// 主循环每秒喂一次。所有判断都在 tick() 里做，ui 只读结果。
struct Signals {
  // 时间是 RTC/NTP 给的，不是编译期默认值。false 时不动 bondDays。
  bool timeReliable = false;
  // 待办停更 / 有坏行。逐条「逾期」的判定要解析 due 串，本期不做（见 .cpp）。
  bool todoTrouble = false;
  // 服务器快照停更。
  bool serverDown = false;
  // 电池端电压 < 3.9 V（ADR-0019 决定 9）。未知时为 false —— 「不知道电压」
  // 不能当成「电压低」，那会让鹅在每个开机头一分钟都不高兴。
  bool lowPower = false;
  // 未完成待办条数；-1 = 还没拿到数据。用来识别「清零那一刻」这个事件。
  int openTodos = -1;
  // 太热：张嘴（ADR-0019 §5 的「温湿度只做细节」）。
  bool hot = false;
  // 距上次按键多久。
  uint32_t idleMs = 0;
};

// 一帧要画的东西。ui.cpp 只认这个结构，不去读全局。
struct View {
  Mood mood = Mood::Idle;
  Why why = Why::None;  // mood 的成因。串口用，屏上不显示。
  bool lookingUp = false;  // 抬头动作正在播（Seen）
  bool blink = false;      // 这一帧闭眼
  bool pant = false;       // 太热：张嘴
  uint8_t breath = 0;      // 呼吸相位 0..3（4 秒一轮）
  uint8_t stride = 0;      // 8 Hz 窗口内的步态相位 0..3；不在窗口内恒为 0
  uint32_t bondDays = 0;   // 一起过了多少天；0 = 还不知道
  uint8_t accessory = 0;   // 0 无 / 2 帽子（≥30 天）。围巾帧已取消。
  // 要画哪一帧。ui.cpp 只认这个编号，不自己推断（帧与帧的差异是「换哪张图」）。
  uint8_t frame = 0;
};

void begin();  // setup() 里调一次：从 NVS 读回 bondDays
void tick(const Signals &s, uint32_t nowMs);
void seen(uint32_t nowMs);  // BOOT 短按：抬头 1.5 s，并开一个动画窗口

View sample(uint32_t nowMs);   // 按当前时刻算一帧
bool burstActive(uint32_t nowMs);  // 8 Hz 窗口是否开着
uint16_t burstStepMs();            // 窗口内帧间隔（125 ms = 8 Hz）

uint32_t bondDays();  // 串口用

// 串口用的 ASCII 名字。屏上一个字都不打（ADR-0019 决定 11）。
const char *moodName(Mood m);
const char *whyName(Why w);

}  // namespace pet
