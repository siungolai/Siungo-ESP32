// rlcd-workbench / PCF85063 RTC
//
// 板载 PCF85063（I2C 0x51），旁边有一颗 PH1.0 可充电备份电池 —— 断电后时间
// 仍然在走，这是本板相对旧 DevKitC 方案最大的不同（那边每次开机都从 1970 开始）。
//
// **存的是 UTC**，不是本地时间。理由：RTC 只认年月日时分秒，它不知道时区；
// 若往里写本地时间，一旦时区有 DST 或者设备被带到别的时区，读回来的东西就
// 只能靠猜。写 UTC 则任何时候读回来都能用内置时区表还原成本地时间。
//
// 星期寄存器（0x08）写进去但读的时候不用 —— 星期由日期现算，避免两者不一致。

#pragma once

#include <stdint.h>

namespace rtc {

bool begin();  // 加设备 + 清 OS 标志 + 起振
bool ready();

// 读回 UTC epoch。寄存器内容不是合法 BCD 或年月日越界时返回 false
// （换过电池/首次上电就是这个样子，调用方要当作「没有可靠时间」处理）。
bool readUtc(int64_t *epoch);

bool writeUtc(int64_t epoch);

// 上电时振荡器是否停过（CTRL1 的 OS 位）。停过 = 掉过电，时间不可信。
bool oscillatorStopped();

}  // namespace rtc
