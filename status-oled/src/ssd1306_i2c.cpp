/**
 * ssd1306_i2c.cpp —— SSD1306 128x64 I2C 裸驱实现
 *
 * 来源：逐字复制 uptime-oled/src/ssd1306_i2c.cpp，刻意差异三处：
 *   1) begin() 去掉末尾的 fontDirProbe()（上电即显，自检交给调用方决定时机）；
 *   2) begin() 做主/备地址回退（0x3C 无应答则试 0x3D），且每次 begin() 都从
 *      主地址重新试探，探通才提交生效地址——回退不会永久污染状态；
 *   3) 所有 I2C 返回值都被检查：初始化中途失联即报失败，运行期靠 flush()
 *      自身已有的 16 次事务判定掉线（零额外总线流量，见 kOfflineFlushFails）。
 *
 * I2C 帧格式（datasheet 第 8 章）：
 *   控制字节 0x00 = 后面全是命令 (Co=0, D/C#=0)
 *   控制字节 0x40 = 后面全是显存数据 (Co=0, D/C#=1)
 */
#include "ssd1306_i2c.h"

#include <Arduino.h>
#include <Wire.h>

#include "font5x7.h"

namespace {

// 每次 I2C 事务的负载上限。ESP32 Arduino 的 Wire 缓冲约 128 字节，
// 单次最多推 1024 字节，所以整帧分成若干块发送。
constexpr size_t kChunkPayload = 64;

// 字模方向自检图案在屏幕上停留多久。放这里是因为它以毫秒计，属于驱动行为；
// 调用方（main.cpp）只决定"何时触发"，停留时长由驱动统一给。
// 原 uptime-oled 把 6000 直接写在函数体里，这里提成常量便于单点调整。
constexpr uint32_t kFontDirProbeMs = 6000;  // 自检画面停留时长，够人看清并判读

}  // namespace

Ssd1306I2c::Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr, uint32_t hz,
                       uint8_t addrAlt)
    : scl_(scl),
      sda_(sda),
      addrPrimary_(addr),
      addr_(addr),
      addrAlt_(addrAlt),
      hz_(hz) {}

bool Ssd1306I2c::cmd(uint8_t c) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // 控制字节：命令
  Wire.write(c);
  return Wire.endTransmission() == 0;
}

bool Ssd1306I2c::cmd2(uint8_t c, uint8_t arg) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // 控制字节：命令
  Wire.write(c);
  Wire.write(arg);
  return Wire.endTransmission() == 0;
}

bool Ssd1306I2c::probe() {
  Wire.beginTransmission(addr_);
  return Wire.endTransmission() == 0;
}

void Ssd1306I2c::setCursor(uint8_t page, uint8_t col) {
  // 页寻址模式下先定位页，再定位列：列低 4 位 + 列高 4 位。
  // 这里刻意丢弃返回值：定位失败必然伴随随后的数据事务失败，
  // 由 flush() 统一判定，不必在每个 setCursor 里重复判错。
  (void)cmd(static_cast<uint8_t>(CMD_SET_PAGE | (page & 0x07)));
  (void)cmd(static_cast<uint8_t>(CMD_SET_LOW_COLUMN | (col & 0x0F)));
  (void)cmd(static_cast<uint8_t>(CMD_SET_HIGH_COLUMN | ((col >> 4) & 0x0F)));
}

bool Ssd1306I2c::begin() {
  // ESP32 上可给 I2C 指定任意 GPIO；先 begin 再探测。
  Wire.begin(sda_, scl_, hz_);
  Wire.setTimeOut(50);  // ms：总线卡死时不要无限等待

  // ---- 地址试探：每次都从主地址重新开始 ----
  // 关键点：用局部变量 chosen 承载"这次探到的地址"，只有真的探通了才写回
  // addr_。原实现直接 addr_ = addrAlt_ 永久改写成员，一次瞬时探测失败
  // （上电时序 / 接触不良 / 模块还没上电）就会把 addr_ 永久污染成 0x3D，
  // 此后每次重试都在错地址上探测，形成只能靠重新上电才能解除的锁死。
  const uint8_t fallback = (addrAlt_ != 0 && addrAlt_ != addrPrimary_)
                               ? addrAlt_
                               : addrPrimary_;  // 等于主地址 = 禁用回退
  uint8_t chosen = addrPrimary_;

  addr_ = addrPrimary_;  // 先无条件回到主地址再探测，保证 begin() 无状态
  bool found = probe();
  if (!found && fallback != addrPrimary_) {
    // 主地址无应答，试一次备用地址（少数模块出厂是 0x3D）。
    // 只有主备都失败才返回 false，交给调用方报告。
    addr_ = fallback;
    found = probe();
    chosen = fallback;
  }
  if (!found) {
    // 两个地址都没应答。把 addr_ 恢复成主地址，避免"失败一次就换地址"
    // 的污染：下一次 begin() 仍然从主地址开始。
    addr_ = addrPrimary_;
    ok_ = false;
    return false;  // 从机没应答：接线 / 地址 / 上拉电阻问题，交给调用方报告
  }
  addr_ = chosen;  // 探通了才提交，此后 addr_ 是"当前生效地址"

  // ---- 上电初始化序列（SSD1306 datasheet 推荐值）----
  // 初始化期间任何一条命令得不到 ACK 都说明从机中途掉了（例如供电跌落），
  // 不能报成功——否则调用方会以为屏幕已就绪。
  bool ok = true;
  ok = cmd(CMD_DISPLAY_OFF) && ok;  // 0xAE 先关显示，避免初始化过程中花屏
  ok = cmd2(CMD_SET_CLOCK_DIV, 0x80) && ok;  // 0xD5 0x80：分频=1，振荡频率档位 8
  ok = cmd2(CMD_SET_MULTIPLEX, OLED_HEIGHT - 1) && ok;  // 0xA8 0x3F：复用率 64 行
  ok = cmd2(CMD_SET_DISPLAY_OFFSET, 0x00) && ok;        // 0xD3 0x00：垂直偏移 0
  ok = cmd(CMD_SET_START_LINE | 0x00) && ok;            // 0x40：显示起始行 0
  ok = cmd2(CMD_CHARGE_PUMP, 0x14) && ok;  // 0x8D 0x14：开内部电荷泵（3.3V 必须开）
  ok = cmd2(CMD_MEMORY_MODE, 0x02) && ok;  // 0x20 0x02：页寻址模式
  ok = cmd(CMD_SEG_REMAP | 0x01) && ok;    // 0xA1：列 127 映射到 SEG0
  ok = cmd(CMD_COM_SCAN_DEC) && ok;        // 0xC8：COM 反向扫描
  ok = cmd2(CMD_SET_COM_PINS, 0x12) && ok;  // 0xDA 0x12：128x64 交替 COM 配置
  ok = cmd2(CMD_SET_CONTRAST, 0xCF) && ok;  // 0x81 0xCF：亮度
  ok = cmd2(CMD_SET_PRECHARGE, 0xF1) && ok;   // 0xD9 0xF1：预充电周期
  ok = cmd2(CMD_SET_VCOM_DETECT, 0x40) && ok;  // 0xDB 0x40：VCOMH 电平
  ok = cmd(CMD_DISPLAY_ALLON_RESUME) && ok;    // 0xA4：按显存内容显示
  ok = cmd(CMD_NORMAL_DISPLAY) && ok;          // 0xA6：正常（非反色）
  ok = cmd(CMD_DEACTIVATE_SCROLL) && ok;       // 0x2E：关滚动
  ok = cmd(CMD_DISPLAY_ON) && ok;              // 0xAF：开显示
  if (!ok) {
    ok_ = false;
    return false;  // 从机在初始化途中失联，让调用方按未上线处理
  }

  clear();
  // 先置 ok_ 再 flush：flush() 对 !ok_ 会直接返回，不置的话这一帧推不出去。
  ok_ = true;
  flushFailStreak_ = 0;
  flush();
  // flush() 会按自己的判据改写 ok_ / flushFailStreak_。首帧这里刻意不走
  // "连续 kOfflineFlushFails 次才判掉线"的滞后判据：地址探测刚刚才成功，
  // 紧接着整帧 16 次事务全失败不是抖动，是从机真的掉了，直接给结论。
  if (flushFailStreak_ != 0) {
    ok_ = false;
    return false;
  }
  // 注意：这里不调用 fontDirProbe()。status-oled 要求上电即显，
  // 字模方向自检改由调用方在用户主动触发时才跑（见 config.h 的
  // FONT_DIR_HOLD_MS / FONT_DIR_PROBE_MS）。
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

  // 掉线检测刻意复用下面这 16 次事务的返回值，不额外插入探测事务：
  // 一次 flush 已占约 16 次事务，再加探测会把 1 秒节拍推向饱和。
  //   anyOk  = 本次 flush 至少有 1 次事务得到 ACK；
  //   全失败 = 记一次 streak，连续 kOfflineFlushFails 次才判定掉线。
  bool anyOk = false;

  for (uint8_t page = 0; page < OLED_PAGES; ++page) {
    // setCursor 的 3 条命令不计入 anyOk：它们不是数据事务，
    // 且失败必然伴随随后数据事务的失败，避免重复计数。
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
      if (Wire.endTransmission() == 0) {
        anyOk = true;
      }
      p += n;
      remaining -= n;
    }
  }

  if (anyOk) {
    // 总线还活着：清掉失败计数，并让 ok_ 回到 true（可自愈）。
    flushFailStreak_ = 0;
    ok_ = true;
    return;
  }

  // 整帧 16 次事务无一成功。单次不计掉线（防偶发 NACK 误判），
  // 连续 kOfflineFlushFails 次才把 ok_ 清掉，交给 main.cpp 走重连分支。
  if (flushFailStreak_ < 0xFF) {
    ++flushFailStreak_;
  }
  if (flushFailStreak_ >= kOfflineFlushFails) {
    ok_ = false;
  }
}

// ------------------------------------------------------------------ 字模方向自检

void Ssd1306I2c::fontDirProbe() {
  // 画两条同长（4 行）的 1x4 竖条，y 起点相隔 14 行。判读方法见 uptime-oled
  // 的 README：只回答"哪条离屏幕垂直中心更远"，不依赖任何字模，
  // 所以检查本身不会跟着一起反。
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
