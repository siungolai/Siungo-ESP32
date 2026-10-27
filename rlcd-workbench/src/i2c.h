// rlcd-workbench / I2C 总线（IDF 新版 i2c_master 驱动）
//
// 这块板子的一条 I2C 上挂了四颗从设备：SHTC3 温湿度 0x70、PCF85063 时钟 0x51、
// ES8311 音频 codec 0x18、ES7210 音频 ADC 0x40。地址互不冲突，可以共用一条总线。
//
// 用 IDF 的 i2c_master 而不是 Arduino 的 Wire：
//   - 官方 BSP 与 05_I2C_SHTC3 例程用的就是这套，语义能一一对上；
//   - Wire 在 Arduino-ESP32 里是旧驱动的包装，多次 begin() 会静默沿用旧引脚
//     （仓库 HARDWARE.md §1.2 记过这个坑，曾经误测 36 小时）。
//
// 参数序：begin(sda, scl)。注意官方 I2cMasterBus 的构造是 (scl, sda)，顺序相反，
// 抄的时候很容易接反 —— 这里把顺序写死在函数签名里，调用处只能按 sda/scl 传。

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <driver/i2c_master.h>

namespace i2cbus {

// 建总线。重复调用是安全的（已建好就直接返回 true）。
bool begin(int sda, int scl);
bool ready();

// 最近一次底层调用的 esp_err_t 原值，失败时打日志用。
int lastError();

// 在总线上挂一个从设备，取得它的句柄。
bool addDevice(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *out);
void removeDevice(i2c_master_dev_handle_t dev);

// 裸的总线句柄（v0.1.5 新增，docs/adr/0022）。
// 存在的唯一理由：esp_codec_dev 的官方 audio_codec_ctrl_i2c.c 要的就是这个类型
// 的句柄，它自己会挂设备、自己发传输。没有访问器的话，声音模块只能另建一条
// I2C —— 同一组引脚上跑两个 master 实例，故障表现为「偶尔读不到 RTC」。
//
// 拿到句柄的调用方**不该**用它绕过 addDevice()/write()/read() 这套封装去做别的事，
// 只该原样交给要它的第三方库。总线未就绪时返回 nullptr。
i2c_master_bus_handle_t busHandle();

int write(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len);
int read(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len);

// 先写后读（写寄存器地址再读值）。SHTC3 用不到，PCF85063 会用到。
int writeRead(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wrLen, uint8_t *rd,
              size_t rdLen);

// 扫描 0x08..0x77，逐条打印是否应答。bringup 用一次就够，正常运行不调用。
void scan();

}  // namespace i2cbus
