// rlcd-workbench / 日期时间工具
//
// 只做纯计算，不碰硬件。M2 接上 PCF85063 RTC 之后，这一层不用改 ——
// 变的只是「时间从哪来」，进位规则还是这一套。
//
// 所有函数都是 inline，避免为了几个小函数多一个编译单元。

#pragma once

#include <stdint.h>

namespace tu {

// 简化日期时间。weekday：1 = 周一 … 7 = 周日（ISO 习惯，与页首/月历一致）。
struct DT {
  int year = 2026;
  int month = 10;
  int day = 22;
  int weekday = 4;
  int hour = 10;
  int minute = 22;
  int second = 31;
};

inline bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

inline int daysInMonth(int y, int m) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 30;
  if (m == 2 && isLeap(y)) return 29;
  return d[m - 1];
}

// 前后挪天，跨月跨年自动进位/退位。
inline void addDays(int &y, int &m, int &d, int n) {
  while (n > 0) {
    if (++d > daysInMonth(y, m)) {
      d = 1;
      if (++m > 12) {
        m = 1;
        ++y;
      }
    }
    --n;
  }
  while (n < 0) {
    if (--d < 1) {
      if (--m < 1) {
        m = 12;
        --y;
      }
      d = daysInMonth(y, m);
    }
    ++n;
  }
}

// 加一秒，逐级进位：秒 → 分 → 时 → 日 → 月 → 年，星期同步滚动。
// 这是本项目唯一一处「时间进位」，M1 之前把它漏了 —— 只更新了秒，
// 于是分和时永远停在开机那一个值上。
inline void addOneSecond(DT &t) {
  if (++t.second < 60) return;
  t.second = 0;

  if (++t.minute < 60) return;
  t.minute = 0;

  if (++t.hour < 24) return;
  t.hour = 0;

  // 跨天：星期先滚（1..7 循环），再进位日期
  t.weekday = (t.weekday >= 1 && t.weekday <= 7) ? (t.weekday % 7) + 1 : 1;
  if (++t.day > daysInMonth(t.year, t.month)) {
    t.day = 1;
    if (++t.month > 12) {
      t.month = 1;
      ++t.year;
    }
  }
}

// 从 1970-01-01 起算的天数 <-> (年,月,日)。
//
// 用 Howard Hinnant 的 days_from_civil / civil_from_days：纯整数、无查表、
// 对负数（1970 年之前）也成立。自己写循环也能算，但跨年累加在 1970..2099
// 这段要跑几万次，而 RTC 与 NTP 每次同步都会调它。
inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

inline void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  y = (int)yoe + (int)era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  d = (int)(doy - (153u * mp + 2u) / 5u + 1u);
  const int mpInt = (int)mp;
  m = mpInt + (mpInt < 10 ? 3 : -9);  // 注意先转 int：无符号与 -9 混算会回绕
  y += (m <= 2);
}

// 1970-01-01 是周四 = ISO 的 4。
inline int weekdayFromDays(int64_t days) { return (int)(((days % 7) + 7 + 3) % 7) + 1; }

// 把一个「当作 UTC 看待」的 DT 折成 epoch 秒。
inline int64_t toEpoch(const DT &t) {
  const int64_t days = daysFromCivil(t.year, (unsigned)t.month, (unsigned)t.day);
  return days * 86400 + (int64_t)t.hour * 3600 + (int64_t)t.minute * 60 + t.second;
}

// epoch 秒折回 DT（weekday 一并算好）。
inline DT fromEpoch(int64_t epoch) {
  int64_t days = epoch / 86400;
  int64_t rem = epoch % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  DT t;
  civilFromDays(days, t.year, t.month, t.day);
  t.weekday = weekdayFromDays(days);
  t.hour = (int)(rem / 3600);
  t.minute = (int)((rem % 3600) / 60);
  t.second = (int)(rem % 60);
  return t;
}

}  // namespace tu
