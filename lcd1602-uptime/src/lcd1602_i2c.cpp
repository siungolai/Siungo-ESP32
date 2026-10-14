/**
 * lcd1602_i2c.cpp —— LCD1602 (HD44780) + PCF8574 I2C 背包 裸驱实现
 *
 * 位映射与硬件背景见 lcd1602_i2c.h 顶部注释。
 *
 * 关键设计：**一次 I2C 传输写完两个 nibble**
 * ---------------------------------------------------------------
 * HD44780 的 4 位模式握手是这样的：把 4 位放到 D4..D7，拉高 E，
 * 保持 >450ns，再拉低 E，数据就被锁存。
 *
 * 教科书写法是把"拉高 E"和"拉低 E"各做成一次 I2C 传输，于是一个字节
 * 要 4 次 I2C 事务。但 PCF8574 允许多个字节写在**一次** I2C 传输里，
 * 按顺序依次锁存到输出口，所以我们可以在一次 beginTransmission /
 * endTransmission 之间把 4 个状态字节按顺序推进去：
 *
 *     1) data | E=1     (E 上升沿，数据摆好)
 *     2) data | E=0     (E 下降沿，锁存)
 *     3) data2 | E=1    (第二个 nibble)
 *     4) data2 | E=0
 *
 * 这样写一个字节只要 1 次 I2C 事务（约 5 字节负载），比朴素写法快 4 倍，
 * 而时序依然满足 datasheet：每个状态之间隔着 I2C 字节传输时间
 * （100kHz 下约 90us），远大于 E 脉冲所需的 450ns。
 *
 * 参考实现对照：mathertel/LiquidCrystal_PCF8574 的 _writeNibble()
 * 正是把 enable 高/低两次 write() 放进同一次 beginTransmission 里。
 */
#include "lcd1602_i2c.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

// HD44780 时序（datasheet 第 4 节 AC Characteristics）
constexpr uint16_t kPulseUs = 1;       // E 脉冲高电平宽度，要求 >450ns
constexpr uint16_t kCmdDelayUs = 40;   // 普通命令执行时间，要求 >37us

// 初始化序列里的长延时（datasheet "Initializing by Instruction"）
constexpr uint16_t kInitDelayMs = 5;   // 上电后等内部复位完成，>4.1ms
constexpr uint16_t kClearDelayMs = 2;  // 清屏/回原点耗时 1.52ms
constexpr uint16_t kLongCmdUs = 1600;  // 清屏用微秒版，1.6ms

}  // namespace

Lcd1602I2c::Lcd1602I2c(uint8_t scl, uint8_t sda, uint8_t addr, uint32_t hz)
    : scl_(scl), sda_(sda), addr_(addr), hz_(hz) {}

// ------------------------------------------------------------------ 底层

bool Lcd1602I2c::probe() {
  Wire.beginTransmission(addr_);
  return Wire.endTransmission() == 0;
}

/**
 * 把当前 shadow_ 原样写进 PCF8574（仅用于背光切换等不需脉冲的场合）。
 * PCF8574 是"写全 8 位"的器件，没有位操作，所以必须整体覆盖。
 */
void Lcd1602I2c::writeRaw(uint8_t value) {
  shadow_ = value;
  if (!ok_) {
    return;
  }
  Wire.beginTransmission(addr_);
  Wire.write(shadow_);
  Wire.endTransmission();
}

/**
 * 送一个 nibble（低 4 位有效）到 D4..D7，并打一次 E 脉冲。
 *
 * 单独调用时每次是一条完整 I2C 事务；sendByte() 里的批量路径不走这里。
 * 保留为 public 是为了让初始化阶段的"裸 nibble"（0x03/0x02）也能复用。
 */
void Lcd1602I2c::writeNibble(uint8_t nibble, bool isData) {
  if (!ok_) {
    return;
  }
  // nibble 从低 4 位搬到 D4..D7（即 bit4..bit7）
  uint8_t data = static_cast<uint8_t>((nibble & 0x0F) << 4);
  if (isData) {
    data |= LCD_BIT_RS;
  }
  // RW 恒为 0（只写不读），这样 P1 一直是低
  if (backlight_) {
    data |= LCD_BIT_BL;
  }

  Wire.beginTransmission(addr_);
  Wire.write(static_cast<uint8_t>(data | LCD_BIT_E));  // E=1
  Wire.write(data);                                    // E=0，下降沿锁存
  Wire.endTransmission();

  delayMicroseconds(kPulseUs);
}

/**
 * 送一个完整字节：高 4 位先、低 4 位后，两次 nibble 合并到一次 I2C 传输。
 *
 * 合并的必要性：一个字符 2 个 nibble，若各自 2 次事务就是 4 次；
 * 整屏 32 个字符就是 128 次 I2C 事务。合并后降到 32 次。
 */
void Lcd1602I2c::sendByte(uint8_t value, bool isData) {
  if (!ok_) {
    return;
  }
  uint8_t base = 0;
  if (isData) {
    base |= LCD_BIT_RS;
  }
  if (backlight_) {
    base |= LCD_BIT_BL;
  }

  const uint8_t hi = static_cast<uint8_t>((value >> 4) & 0x0F);
  const uint8_t lo = static_cast<uint8_t>(value & 0x0F);

  const uint8_t hiData = static_cast<uint8_t>(base | (hi << 4));
  const uint8_t loData = static_cast<uint8_t>(base | (lo << 4));

  Wire.beginTransmission(addr_);
  // 高 4 位：E=1 -> E=0
  Wire.write(static_cast<uint8_t>(hiData | LCD_BIT_E));
  Wire.write(hiData);
  // 低 4 位：E=1 -> E=0
  Wire.write(static_cast<uint8_t>(loData | LCD_BIT_E));
  Wire.write(loData);
  Wire.endTransmission();

  shadow_ = loData;

  // 除清屏/回原点外的命令与所有数据，执行时间都远小于 40us
  delayMicroseconds(kCmdDelayUs);
}

// ------------------------------------------------------------------ 初始化

/**
 * HD44780 的 4 位模式初始化序列（datasheet Figure 24 "Initializing by
 * Instruction"）。要点：
 *
 *   1) 控制器上电后可能是 8 位模式，且不知道我们想切 4 位，
 *      所以先用 0x03 连打三次（在 8 位模式下这三次都被识别成
 *      "Function Set"），再打 0x02 把它切到 4 位模式。
 *   2) 三次 0x03 的间隔必须够长（>4.1ms 后 >100us），datasheet 有明确表。
 *   3) 切到 4 位模式之后，才能用正常的 4 位命令。
 */
void Lcd1602I2c::initSequence() {
  delay(kInitDelayMs);  // 等内部上电复位完成

  // ---- 阶段一：强制进入 4 位模式（此法与 LiquidCrystal 库一致）----
  writeNibble(0x03, false);
  delay(kInitDelayMs);
  writeNibble(0x03, false);
  delayMicroseconds(200);
  writeNibble(0x03, false);
  delayMicroseconds(200);
  writeNibble(0x02, false);  // 切 4 位模式
  delayMicroseconds(200);

  // ---- 阶段二：正式配置 ----
  // 功能设置：4 位总线(0x00) | 2 行(0x08) | 5x8 点阵(0x00) = 0x28
  sendByte(static_cast<uint8_t>(LCD_CMD_FUNCTION_SET | 0x08), false);
  // 显示开关：开显示(0x04) | 关光标(0x00) | 不闪烁(0x00) = 0x0C
  sendByte(static_cast<uint8_t>(LCD_CMD_DISPLAY_CTRL | 0x04), false);
  clear();
  // 入口模式：写一个字符后光标右移(0x02) | 画面不移(0x00) = 0x06
  sendByte(static_cast<uint8_t>(LCD_CMD_ENTRY_MODE | 0x02), false);
}

bool Lcd1602I2c::begin() {
  ok_ = false;
  backlight_ = true;

  Wire.begin(sda_, scl_, hz_);
  Wire.setTimeOut(50);  // ms：总线卡死时不要无限等待

  if (!probe()) {
    return false;  // 从机没应答：接线 / 供电 / 地址问题，交给调用方报告
  }

  initSequence();
  ok_ = true;
  return true;
}

bool Lcd1602I2c::beginAt(uint8_t addr) {
  addr_ = addr;
  return begin();
}

// ------------------------------------------------------------------ 高层

void Lcd1602I2c::clear() {
  sendByte(LCD_CMD_CLEAR, false);
  delay(kClearDelayMs);  // 清屏是全屏操作，datasheet 要求 1.52ms
}

void Lcd1602I2c::home() {
  sendByte(LCD_CMD_HOME, false);
  delay(kClearDelayMs);
}

/**
 * 定位光标。1602 的 DDRAM 是"每行 40 字节"的布局，
 * 第 1 行地址 0x00..0x27，第 2 行地址 0x40..0x67。
 * 所以第 2 行的第 col 列地址 = 0x40 + col。
 */
void Lcd1602I2c::setCursor(uint8_t col, uint8_t row) {
  if (row >= LCD_ROWS) {
    return;
  }
  if (col >= LCD_COLS) {
    col = LCD_COLS - 1;
  }
  sendByte(static_cast<uint8_t>(LCD_CMD_SET_DDRAM | (LCD_ROW_OFFSET[row] + col)),
           false);
}

void Lcd1602I2c::print(const char* s) {
  for (; *s != '\0'; ++s) {
    sendByte(static_cast<uint8_t>(*s), true);
  }
}

/**
 * 定长写一行。这是本项目"刷新不残影"的关键：
 * 1602 写入不会自动清除旧字符，内容变短时尾部会留下上一帧的字符
 * （例如 "TOTAL 10 s" -> "TOTAL 9 s" 会留下 "TOTAL 9 s " 里的残留）。
 * 所以统一补空格到 16 列。
 */
void Lcd1602I2c::printLine(uint8_t row, const char* s) {
  setCursor(0, row);
  uint8_t written = 0;
  for (; *s != '\0' && written < LCD_COLS; ++s, ++written) {
    sendByte(static_cast<uint8_t>(*s), true);
  }
  for (; written < LCD_COLS; ++written) {
    sendByte(' ', true);
  }
}

void Lcd1602I2c::backlight(bool on) {
  if (backlight_ == on) {
    return;
  }
  backlight_ = on;
  // 背光位在 PCF8574 的 P3，不需要 E 脉冲，直接整体写一次即可
  uint8_t value = 0;
  if (on) {
    value |= LCD_BIT_BL;
  }
  writeRaw(value);
}
