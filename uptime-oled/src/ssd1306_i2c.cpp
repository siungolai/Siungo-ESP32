/**
 * ssd1306_i2c.cpp —— SSD1306 128x64 I2C 裸驱实现
 *
 * I2C 帧格式（datasheet 第 8 章）：
 *   控制字节 0x00 = 后面全是命令 (Co=0, D/C#=0)
 *   控制字节 0x40 = 后面全是显存数据 (Co=0, D/C#=1)
 */
#include "ssd1306_i2c.h"

#include <Wire.h>

#include "font5x7.h"

namespace {

// 每次 I2C 事务的负载上限。ESP32 Arduino 的 Wire 缓冲约 128 字节，
// 单次最多推 1024 字节，所以整帧分成若干块发送。
constexpr size_t kChunkPayload = 64;

// 字模方向自检图案在屏幕上停留多久。放这里是因为它以毫秒计，属于驱动行为。
constexpr uint32_t kFontDirProbeMs = 6000;

}  // namespace

Ssd1306I2c::Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr, uint32_t hz)
    : scl_(scl), sda_(sda), addr_(addr), hz_(hz) {}

void Ssd1306I2c::cmd(uint8_t c) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // 控制字节：命令
  Wire.write(c);
  Wire.endTransmission();
}

void Ssd1306I2c::cmd2(uint8_t c, uint8_t arg) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // 控制字节：命令
  Wire.write(c);
  Wire.write(arg);
  Wire.endTransmission();
}

bool Ssd1306I2c::probe() {
  Wire.beginTransmission(addr_);
  return Wire.endTransmission() == 0;
}

void Ssd1306I2c::setCursor(uint8_t page, uint8_t col) {
  // 页寻址模式下先定位页，再定位列：列低 4 位 + 列高 4 位
  cmd(static_cast<uint8_t>(CMD_SET_PAGE | (page & 0x07)));
  cmd(static_cast<uint8_t>(CMD_SET_LOW_COLUMN | (col & 0x0F)));
  cmd(static_cast<uint8_t>(CMD_SET_HIGH_COLUMN | ((col >> 4) & 0x0F)));
}

bool Ssd1306I2c::begin() {
  // ESP32 上可给 I2C 指定任意 GPIO；先 begin 再探测。
  Wire.begin(sda_, scl_, hz_);
  Wire.setTimeOut(50);  // ms：总线卡死时不要无限等待

  ok_ = probe();
  if (!ok_) {
    return false;  // 从机没应答：接线 / 地址 / 上拉电阻问题，交给调用方报告
  }

  // ---- 上电初始化序列（SSD1306 datasheet 推荐值）----
  cmd(CMD_DISPLAY_OFF);        // 0xAE 先关显示，避免初始化过程中花屏
  cmd2(CMD_SET_CLOCK_DIV, 0x80);   // 0xD5 0x80：分频=1，振荡频率档位 8
  cmd2(CMD_SET_MULTIPLEX, OLED_HEIGHT - 1);  // 0xA8 0x3F：复用率 64 行
  cmd2(CMD_SET_DISPLAY_OFFSET, 0x00);        // 0xD3 0x00：垂直偏移 0
  cmd(CMD_SET_START_LINE | 0x00);            // 0x40：显示起始行 0
  cmd2(CMD_CHARGE_PUMP, 0x14);               // 0x8D 0x14：开内部电荷泵（3.3V 供电必须开）
  cmd2(CMD_MEMORY_MODE, 0x02);               // 0x20 0x02：页寻址模式（写满一页不跨页）
  cmd(CMD_SEG_REMAP | 0x01);                 // 0xA1：列 127 映射到 SEG0（左右镜像基准）
  cmd(CMD_COM_SCAN_DEC);                     // 0xC8：COM 反向扫描（上下翻转基准）
  cmd2(CMD_SET_COM_PINS, 0x12);              // 0xDA 0x12：128x64 交替 COM 引脚配置
  cmd2(CMD_SET_CONTRAST, 0xCF);              // 0x81 0xCF：亮度
  cmd2(CMD_SET_PRECHARGE, 0xF1);             // 0xD9 0xF1：预充电周期
  cmd2(CMD_SET_VCOM_DETECT, 0x40);           // 0xDB 0x40：VCOMH 电平
  cmd(CMD_DISPLAY_ALLON_RESUME);             // 0xA4：按显存内容显示
  cmd(CMD_NORMAL_DISPLAY);                   // 0xA6：正常（非反色）
  cmd(CMD_DEACTIVATE_SCROLL);                // 0x2E：关滚动
  cmd(CMD_DISPLAY_ON);                       // 0xAF：开显示

  clear();
  flush();
  fontDirProbe();
  return true;
}

void Ssd1306I2c::clear() { memset(buf_, 0x00, sizeof(buf_)); }

void Ssd1306I2c::flush() {
  // 从机不在线时不要白刷：每次 endTransmission 都会阻塞到超时，
  // 8 页 × 2 次事务能把 loop 拖住近一秒，反过来破坏 1s 的刷新周期。
  // 探测失败后直接跳过，主循环的计时精度不受影响。
  if (!ok_) {
    return;
  }

  for (uint8_t page = 0; page < OLED_PAGES; ++page) {
    setCursor(page, 0);
    const uint8_t* p = buf_ + static_cast<size_t>(page) * OLED_WIDTH;
    uint8_t remaining = OLED_WIDTH;
    while (remaining > 0) {
      const uint8_t n =
          (remaining > kChunkPayload) ? kChunkPayload : remaining;
      Wire.beginTransmission(addr_);
      Wire.write(0x40);  // 控制字节：显存数据
      for (uint8_t i = 0; i < n; ++i) {
        Wire.write(p[i]);
      }
      Wire.endTransmission();
      p += n;
      remaining -= n;
    }
  }
}

// ------------------------------------------------------------------ 字模方向自检

void Ssd1306I2c::fontDirProbe() {
  // 画两条同长（4 行）的 1x4 竖条，y 起点相隔 14 行。判读方法见 font_dir.h：
  // 只回答"哪条离屏幕垂直中心更远"，不依赖任何字模，所以检查本身不会跟着一起反。
  auto vbar = [this](uint8_t x, uint8_t y, uint8_t h) {
    for (uint8_t i = 0; i < h; ++i) {
      if (static_cast<uint16_t>(y) + i >= OLED_HEIGHT) {
        break;
      }
      uint8_t* row = buf_ + static_cast<size_t>((y + i) >> 3) * OLED_WIDTH;
      row[x] |= static_cast<uint8_t>(1u << ((y + i) & 7));
    }
  };

  clear();
  vbar(2, 27, 4);    // 左缘
  vbar(120, 35, 4);  // 右缘
  flush();

  Serial.println();
  Serial.println("=== font-dir self-check (image on screen) ===");
  Serial.println("Two 1x4 bars, 14 rows apart, left edge and right edge.");
  Serial.println("Which bar is FARTHER from the screen's vertical center?");
  Serial.println("  outer bar is UPPER  -> 0xC8 is correct, keep bit0 = top");
  Serial.println("  outer bar is LOWER  -> use CMD_COM_SCAN_NORMAL (0xC0),");
  Serial.println("                         or flip the glyph rows");
  Serial.println("============================================");
  Serial.println();

  delay(kFontDirProbeMs);
  clear();
  flush();
}

void Ssd1306I2c::drawChar5x7(uint8_t x, uint8_t page, char c) {
  if (page >= OLED_PAGES) {
    return;
  }
  const uint8_t* g = glyph5x7(c);
  uint8_t* row = buf_ + static_cast<size_t>(page) * OLED_WIDTH;
  for (uint8_t i = 0; i < 5; ++i) {
    const uint8_t col = x + i;
    if (col < OLED_WIDTH) {
      row[col] = g[i];
    }
  }
  // 第 6 列为字间距：显式清零，避免刷新时残留上一次更长的内容
  const uint8_t gap = x + 5;
  if (gap < OLED_WIDTH) {
    row[gap] = 0x00;
  }
}

void Ssd1306I2c::drawText5x7(uint8_t x, uint8_t page, const char* s) {
  uint8_t cursor = x;
  for (; *s != '\0'; ++s) {
    if (cursor + 5 >= OLED_WIDTH) {
      break;  // 放不下整个字形就连字间距一起放弃，保持右侧干净
    }
    drawChar5x7(cursor, page, *s);
    cursor += 6;
  }
}

void Ssd1306I2c::drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s) {
  if (page + 1 >= OLED_PAGES) {
    return;
  }
  uint8_t cursor = x;
  for (; *s != '\0'; ++s) {
    if (cursor + 10 >= OLED_WIDTH) {
      break;
    }
    const uint8_t* g = glyph5x7(*s);
    // 2 倍放大：源列 i 展开到目标列 2i、2i+1；源 bit b 展开到目标 bit 2b、2b+1。
    // 因目标字形跨两页，低 8 位落在上页、被挤出的高 4 位落在下页。
    uint8_t* upper = buf_ + static_cast<size_t>(page) * OLED_WIDTH;
    uint8_t* lower = upper + OLED_WIDTH;
    for (uint8_t i = 0; i < 5; ++i) {
      uint8_t lo = 0;
      uint8_t hi = 0;
      for (uint8_t b = 0; b < 7; ++b) {
        if (g[i] & (1u << b)) {
          const uint8_t dstBit = b * 2;  // 0,2,4,..,12
          if (dstBit < 8) {
            lo |= static_cast<uint8_t>(1u << dstBit) |
                  static_cast<uint8_t>(1u << (dstBit + 1));
          } else {
            hi |= static_cast<uint8_t>(1u << (dstBit - 8)) |
                  static_cast<uint8_t>(1u << (dstBit - 8 + 1));
          }
        }
      }
      const uint8_t c0 = cursor + i * 2;
      const uint8_t c1 = c0 + 1;
      if (c1 < OLED_WIDTH) {
        upper[c0] = lo;
        upper[c1] = lo;
        lower[c0] = hi;
        lower[c1] = hi;
      }
    }
    // 2 倍放大后字间距 2 列
    const uint8_t gap0 = cursor + 10;
    const uint8_t gap1 = gap0 + 1;
    if (gap1 < OLED_WIDTH) {
      upper[gap0] = 0x00;
      upper[gap1] = 0x00;
      lower[gap0] = 0x00;
      lower[gap1] = 0x00;
    }
    cursor += 12;
  }
}

void Ssd1306I2c::fillPageSpan(uint8_t x0, uint8_t x1, uint8_t page,
                              uint8_t pattern) {
  if (page >= OLED_PAGES || x0 > x1) {
    return;
  }
  uint8_t* row = buf_ + static_cast<size_t>(page) * OLED_WIDTH;
  for (uint8_t x = x0; x <= x1 && x < OLED_WIDTH; ++x) {
    row[x] = pattern;
  }
}
