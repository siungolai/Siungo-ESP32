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

}  // namespace tu
