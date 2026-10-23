// rlcd-workbench / 时间源调度
//
// 三条来源，优先级从高到低：
//   1. NTP（联网时）—— 唯一能给出「现在到底几点」的东西
//   2. PCF85063 RTC —— 断电后仍在走，精度够看，但会日累积漂
//   3. 编译期默认值 —— 两者都没有时的占位，屏上照常走，但不声称准
//
// 政策（见 config.h）：开机一次 + 每 24 小时一次。不是「只跑一次就再也不管」，
// 否则 RTC 的日累积误差会一直涨下去。
//
// RTC 存 UTC，本模块负责 UTC <-> 本地时间的换算（tz 表，newlib 没有 zoneinfo）。

#pragma once

#include <stdint.h>

namespace timesync {

enum class Source : uint8_t {
  None = 0,  // 编译期默认值，不可信
  Rtc = 1,   // 从 PCF85063 读回
  Ntp = 2,   // 本次开机内校过时
};

void begin();  // setup() 里调一次：配时区 + 读 RTC + 灌进 workclock
void tick();   // loop() 里调，非阻塞

bool synced();  // 当前时间是否来自可信来源
Source source();
const char *sourceName();  // 串口用，ASCII

bool rtcValid();   // RTC 上有没有可信时间
bool rtcPresent(); // 芯片在不在总线上

// 下一次 tick 立刻尝试校时（硬件按键以后接，现在只给自检用）。
void requestSync();

}  // namespace timesync
