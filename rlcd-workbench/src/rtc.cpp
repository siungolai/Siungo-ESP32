#include "rtc.h"

#include <driver/i2c_master.h>

#include "config.h"
#include "i2c.h"
#include "timeutil.h"

namespace rtc {
namespace {

// PCF85063 寄存器映射（见 HARDWARE.md §11.6 与官方 04_I2C_PCF85063 例程）
constexpr uint8_t kRegCtrl1 = 0x00;
constexpr uint8_t kRegSecond = 0x04;  // 0x04..0x0A 是 BCD 的年月日时分秒

constexpr uint8_t kCtrl1Stop = 0x20;  // 置 1 = 停振
constexpr uint8_t kCtrl1Os = 0x80;    // 读：1 = 上次上电后振荡器停过

i2c_master_dev_handle_t g_dev = nullptr;
bool g_osStopped = false;

bool bcdValid(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
uint8_t toBcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
int fromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

bool readReg(uint8_t reg, uint8_t *buf, size_t len) {
  return i2cbus::writeRead(g_dev, &reg, 1, buf, len) == 0;
}

bool writeReg(uint8_t reg, const uint8_t *buf, size_t len) {
  uint8_t tmp[16];
  if (len > sizeof(tmp) - 1) return false;
  tmp[0] = reg;
  for (size_t i = 0; i < len; ++i) tmp[i + 1] = buf[i];
  return i2cbus::write(g_dev, tmp, len + 1) == 0;
}

bool stopClock(bool stop) {
  uint8_t ctrl = 0;
  if (!readReg(kRegCtrl1, &ctrl, 1)) return false;
  if (stop) {
    ctrl |= kCtrl1Stop;
  } else {
    ctrl &= (uint8_t)~kCtrl1Stop;
    ctrl &= (uint8_t)~kCtrl1Os;  // 写 0 清掉「停过」标志
  }
  return writeReg(kRegCtrl1, &ctrl, 1);
}

}  // namespace

bool begin() {
  if (!i2cbus::addDevice(cfg::kAddrRtc, cfg::kI2cClockHz, &g_dev)) {
    g_dev = nullptr;
    return false;
  }

  uint8_t ctrl = 0;
  if (readReg(kRegCtrl1, &ctrl, 1)) {
    g_osStopped = (ctrl & kCtrl1Os) != 0;
  }

  // 清 STOP 让时钟走起来；顺手把 OS 标志写 0（否则它一直是「停过」）。
  if (!stopClock(false)) {
    return false;
  }
  return true;
}

bool ready() { return g_dev != nullptr; }

bool oscillatorStopped() { return g_osStopped; }

bool readUtc(int64_t *epoch) {
  if (!epoch || !g_dev) return false;

  uint8_t b[7] = {};
  if (!readReg(kRegSecond, b, sizeof(b))) return false;

  // b[0]=秒 b[1]=分 b[2]=时 b[3]=日 b[4]=星期 b[5]=月 b[6]=年
  for (int i = 0; i < 7; ++i) {
    if (i == 4) continue;  // 星期寄存器不参与 BCD 合法性判断（我们不用它）
    if (!bcdValid(b[i])) return false;
  }

  const int second = fromBcd(b[0]);
  const int minute = fromBcd(b[1]);
  const int hour = fromBcd(b[2]);
  const int day = fromBcd(b[3]);
  const int month = fromBcd(b[5]);
  const int year = 2000 + fromBcd(b[6]);

  if (second > 59 || minute > 59 || hour > 23) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > tu::daysInMonth(year, month)) return false;
  if (year < 2020 || year > 2099) return false;

  tu::DT t;
  t.year = year;
  t.month = month;
  t.day = day;
  t.hour = hour;
  t.minute = minute;
  t.second = second;
  t.weekday = tu::weekdayFromDays(tu::daysFromCivil(year, (unsigned)month, (unsigned)day));
  *epoch = tu::toEpoch(t);
  return true;
}

bool writeUtc(int64_t epoch) {
  if (!g_dev) return false;

  const tu::DT t = tu::fromEpoch(epoch);

  // 写的时候先停钟：否则跨秒写到一半，寄存器可能落在「59 秒 + 下一秒」之间。
  if (!stopClock(true)) return false;

  uint8_t b[7];
  b[0] = toBcd(t.second);
  b[1] = toBcd(t.minute);
  b[2] = toBcd(t.hour);
  b[3] = toBcd(t.day);
  b[4] = (uint8_t)(t.weekday % 7);  // PCF85063 的星期是 0..6，周日 = 0
  b[5] = toBcd(t.month);
  b[6] = toBcd(t.year % 100);

  const bool ok = writeReg(kRegSecond, b, sizeof(b));
  stopClock(false);
  return ok;
}

}  // namespace rtc
