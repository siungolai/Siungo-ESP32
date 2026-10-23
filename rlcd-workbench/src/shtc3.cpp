#include "shtc3.h"

#include <Arduino.h>

#include "config.h"
#include "i2c.h"

namespace shtc3 {
namespace {

i2c_master_dev_handle_t g_dev = nullptr;
uint16_t g_id = 0;

// 命令字（大端两字节）。与官方 i2c_equipment.h 的常量一一对应。
constexpr uint16_t kCmdReadId = 0xEFC8;
constexpr uint16_t kCmdSoftReset = 0x805D;
constexpr uint16_t kCmdSleep = 0xB098;
constexpr uint16_t kCmdWakeup = 0x3517;
// T 先 RH 后 + polling 模式：测量约 12.1 ms，这里等 20 ms。
constexpr uint16_t kCmdMeasTRhPolling = 0x7866;
// RH 先 T 后 + polling。只在诊断里用，见 readRhFirst()。
constexpr uint16_t kCmdMeasRhTPolling = 0x58E0;

// 唤醒后数据手册要求 50 us ~ 240 us 内不能发下一条命令，官方给的是 50 ms。
// 这里沿用官方值：省这点时间没有意义，读错一次要等 60 秒才有下一轮。
constexpr uint32_t kWakeupDelayMs = 50;
constexpr uint32_t kMeasureDelayMs = 20;
constexpr uint32_t kSoftResetDelayMs = 20;
constexpr uint32_t kIdDelayMs = 5;

bool sendCmd(uint16_t cmd) {
  if (g_dev == nullptr) {
    return false;
  }
  const uint8_t buf[2] = {static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd & 0xFF)};
  return i2cbus::write(g_dev, buf, 2) == 0;
}

// Sensirion 的 CRC-8：多项式 x^8 + x^5 + x^4 + 1（0x31），初值 0xFF，无反射、无末尾异或。
// 官方例程写的是 0x131，那是把最高位也算进多项式宽度了，逐位左移时仍是 0x31。
uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

uint16_t readIdRaw() {
  if (!sendCmd(kCmdReadId)) {
    return 0;
  }
  delay(kIdDelayMs);
  uint8_t buf[3] = {};
  if (i2cbus::read(g_dev, buf, 3) != 0) {
    return 0;
  }
  if (crc8(buf, 2) != buf[2]) {
    return 0;
  }
  return static_cast<uint16_t>((buf[0] << 8) | buf[1]);
}

// 一次完整测量的公共实现。tFirst 决定前两字节是温度还是湿度。
bool measure(uint16_t cmd, bool tFirst, Reading *out) {
  if (g_dev == nullptr || out == nullptr) {
    return false;
  }
  *out = Reading{};

  if (!sendCmd(kCmdWakeup)) {
    return false;
  }
  delay(kWakeupDelayMs);

  if (!sendCmd(cmd)) {
    sleep();  // 测量没发出去也要睡，否则下次唤醒前一直在耗电
    return false;
  }
  delay(kMeasureDelayMs);

  uint8_t buf[6] = {};
  const int rc = i2cbus::read(g_dev, buf, 6);
  sleep();  // 读完立刻睡，不等下一轮
  if (rc != 0) {
    return false;
  }

  // 6 字节 = 高字节/低字节/CRC × 2，两段 CRC 都要过。
  if (crc8(buf, 2) != buf[2] || crc8(&buf[3], 2) != buf[5]) {
    return false;
  }

  const uint16_t first = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
  const uint16_t second = static_cast<uint16_t>((buf[3] << 8) | buf[4]);

  out->rawFirst = first;
  out->rawSecond = second;
  out->rawTemp = tFirst ? first : second;
  out->rawHumi = tFirst ? second : first;
  out->tempC = 175.0f * static_cast<float>(out->rawTemp) / 65536.0f - 45.0f;
  out->tempAdjC = out->tempC + cfg::kShtc3TempOffsetC;
  out->humi = 100.0f * static_cast<float>(out->rawHumi) / 65536.0f;
  out->ok = true;
  return true;
}

}  // namespace

bool begin() {
  if (g_dev != nullptr) {
    return true;
  }
  if (!i2cbus::ready()) {
    return false;
  }
  if (!i2cbus::addDevice(cfg::kAddrShtc3, cfg::kI2cClockHz, &g_dev)) {
    g_dev = nullptr;
    return false;
  }

  // 上电后传感器处于休眠态，必须唤醒才能发命令。
  if (!sendCmd(kCmdWakeup)) {
    i2cbus::removeDevice(g_dev);
    g_dev = nullptr;
    return false;
  }
  delay(kWakeupDelayMs);

  sendCmd(kCmdSoftReset);
  delay(kSoftResetDelayMs);

  g_id = readIdRaw();
  return true;
}

bool ready() { return g_dev != nullptr && g_id != 0; }

uint16_t sensorId() { return g_id; }

bool read(Reading *out) { return measure(kCmdMeasTRhPolling, true, out); }

bool readRhFirst(Reading *out) { return measure(kCmdMeasRhTPolling, false, out); }

void sleep() { sendCmd(kCmdSleep); }

}  // namespace shtc3
