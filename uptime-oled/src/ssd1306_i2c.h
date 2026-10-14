/**
 * ssd1306_i2c.h —— SSD1306 128x64 I2C 裸驱
 *
 * 为什么不用现成库：
 *   - Adafruit_SSD1306 自带 1KB 全屏缓冲 + GFX 抽象层，编译慢、依赖重；
 *   - 本任务只需要"写文本 + 一行进度条"，裸驱 30 行命令就能覆盖，
 *     初始化序列按 SSD1306 datasheet 直写，改动完全可控。
 *
 * 显存模型：128x64 分 8 页（page），每页 8 行像素、128 列，
 *           每个字节代表一列里的 8 个垂直像素，bit0 = 页内最上一行。
 *           全屏 = 8 * 128 = 1024 字节。
 */
#pragma once

#include <stdint.h>

// ------------------------------------------------------------------ 常量
static const uint8_t OLED_WIDTH = 128;
static const uint8_t OLED_HEIGHT = 64;
static const uint8_t OLED_PAGES = OLED_HEIGHT / 8;  // 8 页
static const uint16_t OLED_BUFFER_SIZE =
    static_cast<uint16_t>(OLED_WIDTH) * OLED_PAGES;  // 1024 字节

// SSD1306 命令码（仅列出本驱动用到的）
enum Ssd1306Cmd : uint8_t {
  CMD_DISPLAY_OFF = 0xAE,
  CMD_DISPLAY_ON = 0xAF,
  CMD_SET_LOW_COLUMN = 0x00,        // 低 4 位列地址
  CMD_SET_HIGH_COLUMN = 0x10,       // 高 4 位列地址
  CMD_MEMORY_MODE = 0x20,           // 0x00 水平 / 0x01 垂直 / 0x02 页寻址
  CMD_SET_CONTRAST = 0x81,
  CMD_CHARGE_PUMP = 0x8D,           // 0x14 = 开内部电荷泵
  CMD_SET_START_LINE = 0x40,
  CMD_SEG_REMAP = 0xA0,             // |0x01 = 左右翻转
  CMD_SET_MULTIPLEX = 0xA8,
  CMD_DISPLAY_ALLON_RESUME = 0xA4,  // 按显存显示
  CMD_NORMAL_DISPLAY = 0xA6,
  CMD_COM_SCAN_DEC = 0xC8,          // 上下翻转
  CMD_SET_DISPLAY_OFFSET = 0xD3,
  CMD_SET_CLOCK_DIV = 0xD5,
  CMD_SET_PRECHARGE = 0xD9,
  CMD_SET_COM_PINS = 0xDA,
  CMD_SET_VCOM_DETECT = 0xDB,
  CMD_SET_PAGE = 0xB0,              // |页号(0..7)
  CMD_DEACTIVATE_SCROLL = 0x2E,
};

// ------------------------------------------------------------------ 驱动类
class Ssd1306I2c {
 public:
  // scl/sda: ESP32 上可为任意 GPIO；addr: 7 位地址（0x3C 或 0x3D）
  Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x3C,
             uint32_t hz = 400000);

  // 建 I2C、探测从机、跑初始化序列。返回 false 表示从机无应答。
  bool begin();

  bool ready() const { return ok_; }

  // ---- 绘图（只改片内缓冲，不碰 I2C）----
  void clear();
  void drawChar5x7(uint8_t x, uint8_t page, char c);
  void drawText5x7(uint8_t x, uint8_t page, const char* s);
  // 2 倍放大：字形高 14 像素，占用 page 与 page+1 两页
  void drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s);
  void fillPageSpan(uint8_t x0, uint8_t x1, uint8_t page, uint8_t pattern);

  // 把 1024 字节缓冲推到屏幕（8 页 × 分块传输）
  void flush();

  // 字模方向自检：点亮两条相隔 14 行的 1x4 竖条，6 秒后自动清屏。
  // 由 begin() 自动调用。判读方法见同仓库 clock-oled/src/font_dir.h。
  void fontDirProbe();

  // ---- 底层 ----
  void cmd(uint8_t c);
  void cmd2(uint8_t c, uint8_t arg);
  void setCursor(uint8_t page, uint8_t col);
  bool probe();

 private:
  uint8_t scl_;
  uint8_t sda_;
  uint8_t addr_;
  uint32_t hz_;
  bool ok_ = false;
  uint8_t buf_[OLED_BUFFER_SIZE] = {0};
};
