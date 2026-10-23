#include "tz.h"

#include <string.h>

#include "config.h"

namespace tz {
namespace {

// 规则简写。无 DST 的时区把 stdMin / dstMin 写成同一个值，四个规则字段留 0。
constexpr Zone kZones[] = {
    // ---- 无 DST ----
    {"UTC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Shanghai", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Hong_Kong", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Macau", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Taipei", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Singapore", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Kuala_Lumpur", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Manila", 480, 480, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Tokyo", 540, 540, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Seoul", 540, 540, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Bangkok", 420, 420, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Jakarta", 420, 420, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Kolkata", 330, 330, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Karachi", 300, 300, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Dubai", 240, 240, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Asia/Riyadh", 180, 180, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Europe/Moscow", 180, 180, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Europe/Istanbul", 180, 180, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Africa/Johannesburg", 120, 120, 0, 0, 0, 0, 0, 0, 0, 0},
    {"America/Sao_Paulo", -180, -180, 0, 0, 0, 0, 0, 0, 0, 0},
    {"America/Mexico_City", -360, -360, 0, 0, 0, 0, 0, 0, 0, 0},

    // ---- 欧盟规则：3 月最后一个周日 02:00 → 10 月最后一个周日 02:00（标准时）----
    {"Europe/London", 0, 60, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Dublin", 0, 60, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Lisbon", 0, 60, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Paris", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Berlin", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Madrid", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Rome", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Amsterdam", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Brussels", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Vienna", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Stockholm", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Warsaw", 60, 120, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Athens", 120, 180, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Helsinki", 120, 180, 3, 5, 0, 2, 10, 5, 0, 2},
    {"Europe/Kyiv", 120, 180, 3, 5, 0, 2, 10, 5, 0, 2},

    // ---- 北美规则：3 月第 2 个周日 02:00 → 11 月第 1 个周日 02:00（标准时）----
    {"America/New_York", -300, -240, 3, 2, 0, 2, 11, 1, 0, 2},
    {"America/Toronto", -300, -240, 3, 2, 0, 2, 11, 1, 0, 2},
    {"America/Chicago", -360, -300, 3, 2, 0, 2, 11, 1, 0, 2},
    {"America/Denver", -420, -360, 3, 2, 0, 2, 11, 1, 0, 2},
    {"America/Phoenix", -420, -420, 0, 0, 0, 0, 0, 0, 0, 0},
    {"America/Los_Angeles", -480, -420, 3, 2, 0, 2, 11, 1, 0, 2},
    {"America/Vancouver", -480, -420, 3, 2, 0, 2, 11, 1, 0, 2},

    // ---- 南半球：DST 跨年（start 月 > end 月），判断逻辑要反过来 ----
    {"Australia/Sydney", 600, 660, 10, 1, 0, 2, 4, 1, 0, 2},
    {"Australia/Melbourne", 600, 660, 10, 1, 0, 2, 4, 1, 0, 2},
    {"Pacific/Auckland", 720, 780, 9, 5, 0, 2, 4, 1, 0, 2},
};

constexpr int kZoneCount = (int)(sizeof(kZones) / sizeof(kZones[0]));

const Zone *g_current = nullptr;
bool g_valid = false;

bool hasDst(const Zone &z) { return z.dstMin != z.stdMin; }

// 某年某月「第 week 个周 wday」那天的 0 点（当作 UTC 的「天数」看）。
// week == 5 表示最后一个。
int64_t ruleDay(int year, int month, int week, int wday) {
  const int64_t first = tu::daysFromCivil(year, (unsigned)month, 1u);
  int firstWday = tu::weekdayFromDays(first) % 7;  // 0 = 周日
  int day = 1 + ((wday - firstWday + 7) % 7) + (week - 1) * 7;
  if (week >= 5) {
    const int dim = tu::daysInMonth(year, month);
    while (day > dim) day -= 7;
  }
  return first + (day - 1);
}

// 转换时刻的 UTC epoch：规则给的是「本地标准时」的钟点，所以减去 stdMin。
int64_t ruleEpoch(int year, int month, int week, int wday, int hour, const Zone &z) {
  const int64_t days = ruleDay(year, month, week, wday);
  return days * 86400 + (int64_t)hour * 3600 - (int64_t)z.stdMin * 60;
}

bool dstActive(int64_t utcEpoch, const Zone &z) {
  if (!hasDst(z)) return false;

  // 转换年份要按「该时刻在标准时下是哪一年」取，跨年边界才不会用错年份。
  const int64_t stdEpoch = utcEpoch + (int64_t)z.stdMin * 60;
  const tu::DT stdLocal = tu::fromEpoch(stdEpoch);
  const int y = stdLocal.year;

  const int64_t start = ruleEpoch(y, z.sMonth, z.sWeek, z.sWday, z.sHour, z);
  const int64_t end = ruleEpoch(y, z.eMonth, z.eWeek, z.eWday, z.eHour, z);

  if (end > start) {
    // 北半球：DST 区间落在同一年内，直接比区间。
    return utcEpoch >= start && utcEpoch < end;
  }
  // 南半球：DST 跨年 —— 这一年里「年初到 end」和「start 到年底」两段都在夏令时，
  // 中间的深冬不是。不能用「把 end 推到下一年再比区间」，那样七月份会被判成夏令时。
  return utcEpoch >= start || utcEpoch < end;
}

}  // namespace

int count() { return kZoneCount; }
const Zone *at(int i) { return (i >= 0 && i < kZoneCount) ? &kZones[i] : nullptr; }

const Zone *find(const char *n) {
  if (!n) return nullptr;
  for (int i = 0; i < kZoneCount; ++i) {
    if (strcmp(kZones[i].name, n) == 0) return &kZones[i];
  }
  return nullptr;
}

const Zone *current() {
  if (!g_current) setByName(cfg::kTimeZoneName);
  return g_current;
}

bool setByName(const char *n) {
  const Zone *z = find(n);
  if (!z) {
    // 不静默退回 UTC：调用方要能从 valid 看出「这个名字没认出来」。
    g_current = find("UTC");
    g_valid = false;
    return false;
  }
  g_current = z;
  g_valid = true;
  return true;
}

int offsetMinutesAt(int64_t utcEpoch) {
  const Zone *z = current();
  if (!z) return 0;
  return dstActive(utcEpoch, *z) ? z->dstMin : z->stdMin;
}

tu::DT toLocal(int64_t utcEpoch) {
  return tu::fromEpoch(utcEpoch + (int64_t)offsetMinutesAt(utcEpoch) * 60);
}

int64_t toUtc(const tu::DT &local) {
  const int64_t asIfUtc = tu::toEpoch(local);
  // 先用「标准时」猜一次，再用猜到的时刻复查一次 DST —— 转换那一小时附近
  // 需要用真实偏移而不是猜的偏移，两轮足够收敛。
  int64_t guess = asIfUtc - (int64_t)current()->stdMin * 60;
  const int off = offsetMinutesAt(guess);
  return asIfUtc - (int64_t)off * 60;
}

const char *name() { return g_current ? g_current->name : "?"; }

}  // namespace tz
