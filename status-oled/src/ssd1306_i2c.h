/**
 * ssd1306_i2c.h —— SSD1306 128x64 I2C 裸驱
 *
 * 来源：逐字复制 uptime-oled/src/ssd1306_i2c.h，三处刻意差异：
 *   1) begin() 不自动调用 fontDirProbe()（status-oled 要上电即显）；
 *   2) begin() 内部做主/备地址回退（0x3C 失败则试 0x3D），
 *      uptime-oled 只声明了 OLED_ADDR_ALT 却没实现回退；
 *   3) 所有 I2C 返回值都被检查，运行期掉线可被感知（见 kOfflineFlushFails），
 *      但检测复用 flush() 已有的 16 次事务，不额外产生总线流量。
 *
 * 地址回退的无状态性（重要）：begin() 每次都从构造时给的 addr_ 基准
 * （即 OLED_ADDR_PRIMARY）重新试探，只在探测成功后把结果写回 addr_。
 * 这样"某次上电时序 / 接触不良导致主地址瞬时无应答"不会永久污染状态——
 * 下一次 begin() 仍会先试主地址，可自愈。
 *
 * 为什么不用现成库：
 *   - Adafruit_SSD1306 自带 1KB 全屏缓冲 + GFX 抽象层，编译慢、依赖重；
 *   - 本任务只需要"写文本 + 一行分隔线"，裸驱 30 行命令就能覆盖，
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

// ---- 运行期掉线判定 ----
// 判定完全复用 flush() 本来就要发的 16 次事务，不额外增加任何 I2C 流量：
//   - 一次 flush 里全部事务都失败 → 记一次"整帧失败"；
//   - 只要有任意一次成功       → 立即清零（总线活着，单次 NACK 是抖动）。
// 连续 kOfflineFlushFails 次整帧失败才把 ok_ 置回 false。按 1 秒节拍
// 也就是约 2 秒确认掉线，既不会把偶发 NACK 误判成拔线，也不会让
// "屏定格 + 固件仍报 I2C OK"持续下去。
static const uint8_t kOfflineFlushFails = 2;

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
  // scl/sda: ESP32 上可为任意 GPIO；addr: 7 位地址（0x3C 或 0x3D）。
  // addrAlt 给定时，addr 无应答会自动再试一次 addrAlt；默认 0 表示不回退。
  Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x3C,
             uint32_t hz = 400000, uint8_t addrAlt = 0);

  // 建 I2C、探测从机、跑初始化序列。返回 false 表示从机无应答。
  // 每次调用都从主地址重新试探（见类注释的"地址回退的无状态性"），
  // 所以调用方可以放心反复重试，不会把状态越试越坏。
  // 注意：本副本不自动调用 fontDirProbe()——status-oled 要求上电即显，
  // 自检改由 main.cpp 在上电时按住按钮 >= 1.5s 触发（见 config.h）。
  bool begin();

  bool ready() const { return ok_; }

  // 当前生效的 7 位地址：begin() 在主地址无应答时会回退到备用地址，
  // 这里让调用方拿到真实值，避免屏上写 0x3C 而实际是 0x3D。
  // 每次 begin() 都从 OLED_ADDR_PRIMARY 重新试探，所以这个值不会
  // 因为一次瞬时探测失败而永久停留在备用地址上。
  uint8_t address() const { return addr_; }

  // 运行期掉线统计：连续多少次整帧 flush 全部事务失败。
  // 不是"最近一次事务失败"——单次 NACK 属正常抖动，不构成掉线。
  uint8_t consecutiveFlushFails() const { return flushFailStreak_; }

  // ---- 绘图（只改片内缓冲，不碰 I2C）----
  void clear();
  void drawChar5x7(uint8_t x, uint8_t page, char c);
  void drawText5x7(uint8_t x, uint8_t page, const char* s);
  // 2 倍放大：字形高 14 像素，占用 page 与 page+1 两页
  void drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s);
  void fillPageSpan(uint8_t x0, uint8_t x1, uint8_t page, uint8_t pattern);

  // 把 1024 字节缓冲推到屏幕（8 页 × 分块传输）
  void flush();

  // 字模方向自检：点亮两条相隔 14 行的 1x4 竖条，kFontDirProbeMs 后自动清屏。
  // 本副本不再由 begin() 自动调用，由调用方决定何时跑（status-oled：按住按钮）。
  void fontDirProbe();

  // ---- 底层 ----
  // 返回 false 表示该事务未得到 ACK（返回值 2/3/4/5）；调用方可据此判断在线。
  bool cmd(uint8_t c);
  bool cmd2(uint8_t c, uint8_t arg);
  void setCursor(uint8_t page, uint8_t col);
  bool probe();

 private:
  uint8_t scl_;
  uint8_t sda_;
  // addrPrimary_ 是构造时给的基准地址，begin() 每次探测都从它重新开始；
  // addr_ 是"当前生效"地址，只有探测成功才会被改写。两者分开是 P0-3 的修法：
  // 把基准和生效值混成一个成员，一次瞬时失败就会永久锁死在备用地址上。
  uint8_t addrPrimary_;
  uint8_t addr_;      // 当前生效地址；仅在探测成功后才被改写
  uint8_t addrAlt_;  // 主地址无应答时试这个；等于 addrPrimary_ 表示禁用回退
  uint32_t hz_;
  bool ok_ = false;
  // 连续"整帧全部事务失败"的次数，达 kOfflineFlushFails 即判定掉线。
  uint8_t flushFailStreak_ = 0;
  uint8_t buf_[OLED_BUFFER_SIZE] = {0};
};