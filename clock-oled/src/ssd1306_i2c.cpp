/**
 * ssd1306_i2c.cpp —— SSD1306 128x64 I2C 裸驱实现
 *
 * 本文件从同仓库的 uptime-oled 原样沿用（本机已实测点亮）。
 *
 * I2C 帧格式（datasheet 第 8 章）：
 *   控制字节 0x00 = 后面全是命令 (Co=0, D/C#=0)
 *   控制字节 0x40 = 后面全是显存数据 (Co=0, D/C#=1)
 */
#include "ssd1306_i2c.h"

#include <Wire.h>

#include "font5x7.h"
#include "font_cn.h"

namespace {

// 每次 I2C 事务的负载上限。ESP32 Arduino 的 Wire 缓冲约 128 字节，
// 单次最多推 1024 字节，所以整帧分成若干块发送。
constexpr size_t kChunkPayload = 64;

// 字模方向自检图案停留时长（毫秒）。
constexpr uint32_t kFontDirProbeMs = 6000;

// 在汉字表里找这个字，返回它的 32 字节点阵；找不到返回 nullptr。
// 表只有 29 个字，线性扫描比建哈希表划算得多。
const uint8_t* cnGlyph(uint32_t codepoint) {
  for (uint16_t i = 0; i < kFontCnCount; ++i) {
    const char* p = kFontCnChars + i * 3;  // 汉字在 UTF-8 里固定占 3 字节
    const uint32_t cp = (static_cast<uint32_t>(p[0] & 0x0F) << 12) |
                        (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
                        (static_cast<uint32_t>(p[2] & 0x3F));
    if (cp == codepoint) {
      return kFontCn[i];
    }
  }
  return nullptr;
}

// 从 UTF-8 串里取出一个码点，并把指针往后挪。
// 只处理 1~3 字节的序列（ASCII / 汉字都在这个范围），足够本程序用。
uint32_t utf8Next(const char*& s) {
  const uint8_t c = static_cast<uint8_t>(*s);
  if (c < 0x80) {           // ASCII
    ++s;
    return c;
  }
  if ((c & 0xE0) == 0xC0) { // 2 字节
    const uint32_t cp = ((c & 0x1F) << 6) |
                        (static_cast<uint8_t>(s[1]) & 0x3F);
    s += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0) { // 3 字节（汉字）
    const uint32_t cp = ((c & 0x0F) << 12) |
                        ((static_cast<uint8_t>(s[1]) & 0x3F) << 6) |
                        (static_cast<uint8_t>(s[2]) & 0x3F);
    s += 3;
    return cp;
  }
  ++s;  // 4 字节及以上：本程序用不到，跳过了事
  return 0;
}

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

// ------------------------------------------------------------------ 字模方向自检

void Ssd1306I2c::fontDirProbe() {
  // 只画竖条，不碰字体。理由见 src/font_dir.h：横幅若用 16x16 字模来画，
  // 就等于"用可能画反的笔去写检查方向的字"，检查本身会失真。
  // 两条同长（4 行）的竖条，垂直相距 14 行；屏幕上"离屏幕中心远的那条在上/下"
  // 就是屏幕自己给的证据，跟字模取向无关。
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
  vbar(2, 27, 4);    // 左缘，靠上
  vbar(120, 35, 4);  // 右缘，靠下
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

// ------------------------------------------------------------------ 绘制
//
// 垂直定位：所有绘制函数都以"像素行 y"为参数，而不是页号。
//   早期版本用页号，因为 SSD1306 的显存按页组织、刷屏也按页走。
//   但一个字形常常高 14~16 像素、必须跨页，若只能从页边界起画，
//   行距就只能是 8 的整数倍，64 行的屏幕会被迫留出整页空白。
//   改成像素行之后，行距可以是任意值。
//
// 内部实现统一走 drawBits：把一个字形按列贴到缓冲的任意 y 位置，
// 该列跨越哪几页就分别 OR 进去。所有字形函数共用这一处位运算，
// 不必各自重复处理跨页。

void Ssd1306I2c::drawBits(uint8_t x, uint8_t y, const uint8_t* cols, uint8_t n,
                          uint8_t rowsHeight) {
  // cols 是"一列一字节"的点阵，共 n 列、每列 rowsHeight 行（8 或 16）。
  // 16 行时按低字节在前、高字节在后排列，与汉字字模一致。
  const uint8_t bytesPerCol = static_cast<uint8_t>(rowsHeight / 8);
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t col = x + i;
    if (col >= OLED_WIDTH) {
      break;
    }
    for (uint8_t b = 0; b < rowsHeight; ++b) {
      const uint8_t byte = cols[i * bytesPerCol + b / 8];
      if ((byte & (1u << (b % 8))) == 0) {
        continue;  // 暗点不写，保留缓冲里已有的内容
      }
      const uint16_t ty = static_cast<uint16_t>(y) + b;
      if (ty >= OLED_HEIGHT) {
        break;
      }
      buf_[static_cast<size_t>(ty / 8) * OLED_WIDTH + col] |=
          static_cast<uint8_t>(1u << (ty % 8));
    }
  }
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

uint8_t Ssd1306I2c::drawText5x7Y(uint8_t x, uint8_t y, const char* s) {
  uint8_t cursor = x;
  for (; *s != '\0'; ++s) {
    if (cursor + 5 > OLED_WIDTH) {
      break;
    }
    const uint8_t* g = glyph5x7(*s);
    // 逐个像素纵向定位：亮点写进对应页，暗点不覆盖已有内容。
    for (uint8_t i = 0; i < 5; ++i) {
      const uint8_t col = cursor + i;
      for (uint8_t b = 0; b < 7; ++b) {
        if ((g[i] & (1u << b)) == 0) {
          continue;
        }
        const uint16_t ty = static_cast<uint16_t>(y) + b;
        if (ty >= OLED_HEIGHT) {
          break;
        }
        buf_[static_cast<size_t>(ty / 8) * OLED_WIDTH + col] |=
            static_cast<uint8_t>(1u << (ty % 8));
      }
    }
    cursor += 6;
  }
  return cursor;
}

uint8_t Ssd1306I2c::drawText5x7(uint8_t x, uint8_t page, const char* s) {
  return drawText5x7Y(x, static_cast<uint8_t>(page * 8), s);
}

void Ssd1306I2c::drawText5x7Scaled2xY(uint8_t x, uint8_t y, const char* s) {
  uint8_t cursor = x;
  for (; *s != '\0'; ++s) {
    if (cursor + 10 > OLED_WIDTH) {
      break;
    }
    const uint8_t* g = glyph5x7(*s);
    // 2 倍放大：源列 i -> 目标列 2i,2i+1；源 bit b -> 目标像素行 2b,2b+1。
    for (uint8_t i = 0; i < 5; ++i) {
      for (uint8_t b = 0; b < 7; ++b) {
        if ((g[i] & (1u << b)) == 0) {
          continue;
        }
        const uint8_t srcBit = static_cast<uint8_t>(b * 2);  // 0,2,4,...,12
        for (uint8_t dy = 0; dy < 2; ++dy) {
          const uint16_t ty = static_cast<uint16_t>(y) + srcBit + dy;
          if (ty >= OLED_HEIGHT) {
            continue;
          }
          for (uint8_t dx = 0; dx < 2; ++dx) {
            const uint8_t col = cursor + i * 2 + dx;
            if (col >= OLED_WIDTH) {
              continue;
            }
            buf_[static_cast<size_t>(ty / 8) * OLED_WIDTH + col] |=
                static_cast<uint8_t>(1u << (ty % 8));
          }
        }
      }
    }
    cursor += 12;
  }
}

void Ssd1306I2c::drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s) {
  drawText5x7Scaled2xY(x, static_cast<uint8_t>(page * 8), s);
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

// ------------------------------------------------------------------ 中文
//
// 汉字 16 像素高，必须能落在任意像素行（见文件顶部"垂直定位"说明）。
// 字模布局：每列 2 字节，低字节是上半（行 0..7），高字节是下半（行 8..15）。

void Ssd1306I2c::drawCnCharY(uint8_t x, uint8_t y, uint32_t codepoint) {
  // 先把该字还原成 16 列 × 16 行的点阵，再交给通用贴图函数。
  // 直接在这里逐像素算也可以，但那会把跨页位运算重复一遍；
  // 16 个字节的临时数组足够小，编译器会放进寄存器/栈里。
  uint8_t cols[16];
  const uint8_t* g = cnGlyph(codepoint);
  if (g == nullptr) {
    // 字库里没有这个字：画一个空心方框占位。
    // 比什么都不画强——空白会让人以为是渲染 bug，方框一看就知道是缺字。
    for (uint8_t i = 0; i < 16; ++i) {
      const bool edge = (i == 0 || i == 15);
      cols[i] = edge ? 0xFF : 0x81;
    }
    drawBits(x, y, cols, 16, 8);   // 上半
    drawBits(x, y + 8, cols, 16, 8);  // 下半用同样的框
    return;
  }

  // 上半页：字模前 16 字节，一列一个字节
  drawBits(x, y, g, 16, 8);
  // 下半页：字模后 16 字节
  drawBits(x, static_cast<uint8_t>(y + 8), g + 16, 16, 8);
}

void Ssd1306I2c::drawCnChar(uint8_t x, uint8_t page, uint32_t codepoint) {
  drawCnCharY(x, static_cast<uint8_t>(page * 8), codepoint);
}

uint8_t Ssd1306I2c::drawTextCnY(uint8_t x, uint8_t y, const char* s) {
  uint8_t cursor = x;
  while (*s != '\0') {
    if (cursor + 16 > OLED_WIDTH) {
      break;  // 右边放不下整个字了，剩下的不再画
    }
    const uint32_t cp = utf8Next(s);
    drawCnCharY(cursor, y, cp);
    cursor += 16;  // 汉字固定 16 列宽，字间不留额外空隙（点阵自带边距）
  }
  return cursor;
}

uint8_t Ssd1306I2c::drawTextCn(uint8_t x, uint8_t page, const char* s) {
  return drawTextCnY(x, static_cast<uint8_t>(page * 8), s);
}

uint8_t Ssd1306I2c::cnTextWidth(const char* s) {
  uint8_t w = 0;
  while (*s != '\0') {
    utf8Next(s);
    w += 16;
  }
  return w;
}
