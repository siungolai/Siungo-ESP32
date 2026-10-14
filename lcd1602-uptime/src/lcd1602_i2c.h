/**
 * lcd1602_i2c.h —— LCD1602 (HD44780) + PCF8574 I2C 背包 裸驱
 *
 * 为什么不用现成库（LiquidCrystal_I2C / LiquidCrystal_PCF8574）：
 *   - 本任务只需要"定位光标 + 写两行文本"，裸驱约 200 行就够；
 *   - 1602 的时序要求（E 脉冲宽度、清屏 1.52ms、初始化 4.5ms）直接写在
 *     代码里，排查问题时看得见，不用翻库源码；
 *   - 零第三方依赖，只用 Arduino core 自带的 Wire，与同仓库 uptime-oled 一致。
 *
 * ---------------------------------------------------------------------------
 * 硬件链路（重要）
 * ---------------------------------------------------------------------------
 *   ESP32-S3 --I2C--> PCF8574 背包 --并口--> HD44780 控制器 --驱动--> 1602 液晶
 *
 * 1602 本身是"并口屏"，不能直接接 I2C。市售"1602A 支持 IIC"的模块，
 * 都是在屏背面焊了一块 PCF8574 转接板，把 8 根并行线压成 2 根 I2C 线。
 * 所以真正对话的芯片是 **PCF8574**，不是 LCD。
 *
 * PCF8574 的 8 个 IO 到 LCD 引脚的映射（业界统一，本驱动按此写死）：
 *
 *   位   PCF8574 引脚   接到 LCD 的     含义
 *   ---------------------------------------------------------------
 *   P0       P0         RS             0=命令  1=数据
 *   P1       P1         RW             0=写    1=读（本驱动恒写 0）
 *   P2       P2         E              使能，高电平期间数据被锁存
 *   P3       P3         BKL(背光)      1=开背光  0=关
 *   P4       P4         D4             数据高 4 位
 *   P5       P5         D5
 *   P6       P6         D6
 *   P7       P7         D7
 *
 * HD44780 用 **4 位模式**：一个字节拆成高 4 位、低 4 位两次送，
 * 每次都要拉一次 E 脉冲。所以写 1 个字符 = 2 次 I2C 事务
 * （本驱动把两次 nibble 合进同一次 I2C 传输，见 .cpp 的说明）。
 *
 * ---------------------------------------------------------------------------
 * 5V 供电与电平转换（本项目最大风险点，务必先读）
 * ---------------------------------------------------------------------------
 * 1602 是 5V 器件，接 3.3V 时对比度通常严重不足。所以本项目按
 * **5V 供电**设计（与你确认的方案一致）。但 ESP32-S3 的 IO 是 3.3V 电平，
 * **不耐 5V 输入**（绝对最大额定值 VDD+0.3V ≈ 3.6V）。
 *
 * PCF8574 背包板上一般自带 4.7k 上拉电阻，上拉到背包的 VCC。
 * 如果背包 VCC 接 5V，那么 SDA/SCL 的 HIGH 电平就是 5V，
 * **直连 ESP32 会通过 IO 的 ESD 保护二极管长期灌电流**，属于慢性损坏。
 *
 * 正确接法（二选一，详见 README）：
 *   A. 背包 VCC 接 3.3V，背光单独处理 —— 不推荐，1602 显影会淡；
 *   B. 背包 VCC 接 5V，SDA/SCL 经双向电平转换模块再进 ESP32 —— 推荐。
 *
 * 本驱动只关心 I2C 字节，与上述电平方案无关；但接错会损坏 IO。
 */
#pragma once

#include <stdint.h>

// ------------------------------------------------------------------ 常量
static const uint8_t LCD_COLS = 16;  // 1602 = 16 列
static const uint8_t LCD_ROWS = 2;   // 1602 = 2 行

// PCF8574 背包的位定义
static const uint8_t LCD_BIT_RS = 0x01;  // P0
static const uint8_t LCD_BIT_RW = 0x02;  // P1
static const uint8_t LCD_BIT_E = 0x04;   // P2
static const uint8_t LCD_BIT_BL = 0x08;  // P3 背光

// HD44780 命令码（4 位模式下只用于解释高字节，实际发送走 nibble）
enum Hd44780Cmd : uint8_t {
  LCD_CMD_CLEAR = 0x01,           // 清屏，耗时 1.52ms
  LCD_CMD_HOME = 0x02,            // 光标回原点，耗时 1.52ms
  LCD_CMD_ENTRY_MODE = 0x04,      // 入口模式设置
  LCD_CMD_DISPLAY_CTRL = 0x08,    // 显示开/关控制
  LCD_CMD_SHIFT = 0x10,           // 光标/显示移位
  LCD_CMD_FUNCTION_SET = 0x20,    // 功能设置
  LCD_CMD_SET_CGRAM = 0x40,       // 写 CGRAM
  LCD_CMD_SET_DDRAM = 0x80,       // 写 DDRAM 地址（定位光标）
};

// 第二行 DDRAM 起始地址。1602 是"每行 40 字节"的控制器，第 2 行从 0x40 起。
static const uint8_t LCD_ROW_OFFSET[LCD_ROWS] = {0x00, 0x40};

// ------------------------------------------------------------------ 驱动类
class Lcd1602I2c {
 public:
  /**
   * @param scl/sda  ESP32 上可为任意 GPIO
   * @param addr     PCF8574 的 7 位地址（0x27 最常见，0x3F 次之）
   * @param hz       I2C 速率。PCF8574 标称只支持 100kHz；
   *                 实测 400kHz 通常也能跑，但 100kHz 更稳，默认取 100k。
   */
  Lcd1602I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x27,
             uint32_t hz = 100000);

  /** 建 I2C、探测从机、跑 HD44780 初始化序列。false = 从机无应答。 */
  bool begin();

  bool ready() const { return ok_; }
  uint8_t address() const { return addr_; }

  /** 换一个地址重试（自动扫描用）。会重建 I2C 并重跑初始化。 */
  bool beginAt(uint8_t addr);

  // ---- 高层操作 ----
  void clear();
  void home();
  void setCursor(uint8_t col, uint8_t row);
  void print(const char* s);

  /** 背光开关。关背光后字符仍存在 DDRAM 里，再开就回来了。 */
  void backlight(bool on);
  bool backlightOn() const { return backlight_; }

  /**
   * 写一整行，右侧用空格补齐到 16 列。
   * 1602 不会自动清掉残留字符，所以定长覆盖是必须的——
   * 否则 "TOTAL 9 s" 缩短成 "TOTAL 8 s" 时尾部会留一个 's'。
   */
  void printLine(uint8_t row, const char* s);

  // ---- 底层 ----
  void writeNibble(uint8_t nibble, bool isData);
  void sendByte(uint8_t value, bool isData);
  void writeRaw(uint8_t value);
  bool probe();

 private:
  void initSequence();

  uint8_t scl_;
  uint8_t sda_;
  uint8_t addr_;
  uint32_t hz_;
  bool ok_ = false;
  bool backlight_ = true;

  /**
   * PCF8574 的输出锁存影子寄存器。
   * PCF8574 没有读改写能力：每次写都是覆盖全部 8 位，
   * 所以 RS/E/BL 的状态必须在本地记住，写 nibble 时一起拼出去。
   */
  uint8_t shadow_ = 0;
};
