// rlcd-workbench / SHTC3 温湿度传感器
//
// 地址 0x70，挂在本板那条共享 I2C 上（SDA=13 / SCL=14）。
//
// 仓库里的 SensorLib 只有 PCF85063 与 RTC helper，**没有 SHTC3 驱动**，
// 所以这一层是照着官方 05_I2C_SHTC3 例程（i2c_equipment.cpp）重写的：
// 命令字、CRC 多项式、测量时序都与例程一致，只有两处不同 ——
//   1. 例程读完成功也把传感器留在唤醒态，这里读完立刻发 Sleep（SHTC3 唤醒态
//      典型电流远高于休眠态，桌面常驻设备没必要一直醒着）；
//   2. 温度同时给出「数据手册值」和「官方补偿值」，见 config.h 的说明。

#pragma once

#include <stdint.h>

namespace shtc3 {

struct Reading {
  bool ok = false;
  uint16_t rawTemp = 0;  // 寄存器原始值，CRC 已校验
  uint16_t rawHumi = 0;
  // 芯片实际返回的第一对 / 第二对，未经任何解释。
  // 判定字节序靠这两个数：T 优先与 RH 优先两条命令如果互换，说明命令字有效。
  uint16_t rawFirst = 0;
  uint16_t rawSecond = 0;
  float tempC = 0.0f;     // 数据手册公式：175*raw/65536 - 45
  float tempAdjC = 0.0f;  // 再叠加官方 -4 °C 补偿
  float humi = 0.0f;      // 相对湿度 %
};

// 建 I2C 设备句柄，然后唤醒 → 软复位 → 读 ID。失败返回 false。
bool begin();
bool ready();

// 产品 ID（读 0xEFC8 回来的 16 位，CRC 已校验）。失败时是 0。
uint16_t sensorId();

// 唤醒 → 触发测量 → 等 20 ms → 读 6 字节 → 校验两段 CRC → 立刻 Sleep。
// 返回 false 的原因看 i2cbus::lastError()。
bool read(Reading *out);

// 诊断用：换用「湿度优先」的命令（0x58E0）再量一次。
//
// SHTC3 有四种测量命令 = {温度优先, 湿度优先} × {polling, clock stretching}。
// 如果两条命令返回的两组 16 位值恰好互换了位置，说明芯片确实在按命令字决定
// 谁先返回 —— 那么「温度优先」读出来的那个数就是真温度，读数与体感对不上
// 只能是自发热，不可能是字节序搞反。§7 第 9 项就是靠这个判定的。
bool readRhFirst(Reading *out);

void sleep();

}  // namespace shtc3
