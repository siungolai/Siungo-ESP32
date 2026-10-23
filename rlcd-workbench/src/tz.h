// rlcd-workbench / 内置时区表
//
// 为什么自己带一张表：ESP32 的 newlib 里**没有 zoneinfo 数据文件**
// （实测 `libnewlib.a` 里搜 `TZif` / `zoneinfo` / `tzfile` 零命中），
// 而 `configTzTime()` 内部只是 `setenv("TZ", ...); tzset();` ——
// 它认的是 POSIX TZ 串（如 `CST-8`），不认 `Asia/Shanghai` 这种名字。
//
// 所以：SNTP 只负责给 UTC，本地化由这张表算。表里存「标准时偏移 + DST 规则」，
// 规则用「某月第 N 个周几 + 本地标准时几点」表达 —— 够覆盖欧/美/澳/新西兰，
// 也不需要每年更新的数据文件。
//
// 边界：表里没有的时区回落到 UTC+0 并置 valid=false，调用方应当据此提示，
// 而不是静默当成 UTC。

#pragma once

#include <stdint.h>

#include "timeutil.h"

namespace tz {

struct Zone {
  const char *name;
  int16_t stdMin;  // 标准时相对 UTC 的分钟数（东八区 = +480）
  int16_t dstMin;  // 夏令时偏移；无 DST 的时区与 stdMin 相等

  // 进入 / 退出 DST 的规则。
  // sWeek / eWeek：1..4 = 该月第 N 个，5 = 最后一个
  // sWday / eWday：0 = 周日 … 6 = 周六
  // sHour / eHour：转换发生在**本地标准时**的几点
  uint8_t sMonth, sWeek, sWday, sHour;
  uint8_t eMonth, eWeek, eWday, eHour;
};

int count();
const Zone *at(int i);
const Zone *find(const char *name);

// 编译期选定的默认时区（见 config.h 的 kTimeZoneName）。
const Zone *current();
bool setByName(const char *name);

// 某个 UTC 时刻本时区的偏移（分钟）。DST 已算进去。
int offsetMinutesAt(int64_t utcEpoch);

// UTC epoch <-> 本地 DT。
tu::DT toLocal(int64_t utcEpoch);
int64_t toUtc(const tu::DT &local);

const char *name();

}  // namespace tz
