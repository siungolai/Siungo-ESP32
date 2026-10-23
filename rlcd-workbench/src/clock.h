// rlcd-workbench / 工作时钟
//
// M1 里时间是一次性灌进去的假数据，秒在跳、分和时永远不动——因为只更新了
// second 字段。把「进位」这件事单独拆出来放这里，主程序只负责告诉它
// 「又过了几秒」，剩下的秒→分→时→日→月→年逐级进位由 tu::addOneSecond 做。
//
// M2 接 PCF85063 RTC 时，只需要在 begin() 里灌入 RTC 读到的真实时间，
// 再把每秒一次的 advance(1) 保留；NTP 校正时直接把偏移写回来即可。

#pragma once

#include <stdint.h>

#include "timeutil.h"

namespace workclock {

// 设定基准时间（开机 / RTC 读回 / NTP 校正都走这里）。
void begin(const tu::DT &t);

// 走时：seconds 可以大于 1 —— 主循环若被长任务阻塞过，跨过去的秒要补齐，
// 否则慢下来的不只是画面，是时间本身。
// 上限 86400 秒（一整天）：真卡那么久说明别处已经坏了，不必真的空转一天。
void advance(uint32_t seconds);

// 当前时间（只读）。
const tu::DT &now();

}  // namespace workclock
