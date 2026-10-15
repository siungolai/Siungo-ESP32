/**
 * config.h —— status-oled 的全部可调常量。
 *
 * 引脚与节拍集中在这里，接线变了只改这一个文件，不动 main.cpp。
 * 命名约定：常量全部 static const，分组用小写注释行隔开。
 */
#pragma once

#include <stdint.h>

namespace config {

// ------------------------------------------------------------------ I2C（OLED）
// 按本机实测：GPIO4=SCL / GPIO5=SDA 可点亮，GPIO1/2 全段超时不通，
// GPIO19/20 是原生 USB 不能占。见 HARDWARE.md §1.2、§2.3。
static const uint8_t I2C_SCL_PIN = 4;
static const uint8_t I2C_SDA_PIN = 5;
static const uint32_t I2C_HZ = 400000;  // SSD1306 标称上限，见 §2.1

// 从机地址：模块背面丝印 0x78 是"含读写位的 8 位写法"，7 位地址是 0x3C。
// 少数模块出厂为 0x3D（丝印 0x7A）。驱动 begin() 先探主地址，失败再探备用地址。
static const uint8_t OLED_ADDR_PRIMARY = 0x3C;
static const uint8_t OLED_ADDR_ALT = 0x3D;

// 单次 I2C 事务超时不在库里配：驱动 ssd1306_i2c.cpp 内部 Wire.setTimeOut(50)，
// 改它要动驱动副本，所以这里不放常量以免两处各写一份。

// ------------------------------------------------------------------ 按钮（翻页）
// GPIO7 不是 ESP32-S3 的 strapping 脚（strapping 是 GPIO0/3/45/46），做数字输入本身没问题。
// 注意：GPIO7 同时是 dht11-oled 的 DHT11 数据脚，两个项目同一时间只能插一个。
// 见 HARDWARE.md §1.2 与 status-oled/README.md 的「与 dht11-oled 的冲突」。
static const uint8_t PIN_BUTTON_PAGE = 7;

// INPUT_PULLUP：按钮内部没有上拉电阻，用内部上拉，按下接地（低有效）。
static const bool BUTTON_PULLUP = true;
static const uint32_t BUTTON_DEBOUNCE_MS = 30;  // 软件消抖窗口

// ------------------------------------------------------------------ 节拍
static const uint32_t REFRESH_MS = 1000;          // 数值刷新周期 1s（Q1-A）
static const uint32_t RESCAN_INTERVAL_MS = 10000;  // 总线静默重扫间隔 10s（Q10-B）
static const uint32_t RETRY_MS = 5000;  // 屏幕掉线后的重试间隔

// ------------------------------------------------------------------ 版面
// 四页，按钮循环切换，不自动轮播（Q2 的自动轮播在 Q9 被手动翻页取代）。
// 每页 8 行：第 0 行页眉 + 第 1..5 行数据，第 6 行留空白；
// 第 7 行不是空白——它放了右下角 SIUNGOLAI 水印（见下面的 ROW_LOGO）。
// 5×7 字模每字符占 6 列，128/6 = 21，所以整行上限 21 字符；
// MAC 那行「MAC 12:34:56:78:9A:BC」正好 21 字符贴边，是全屏最长的一行。
// （文档里出现的 MAC 一律是虚构示例，真实值只来自板上运行时。）
//
// 标签固定占 4 字符，数值从第 5 列起，所以数值列上限 21 - 4 = 17 字符。
// 所有 render 函数都靠这两个常量算可用宽度，改字模宽度只改这里。
static const uint8_t PAGE_COUNT = 4;
static const uint8_t ROW_PAGE = 0;    // 页眉行
static const uint8_t ROW_FIRST = 1;   // 第一行数据
static const uint8_t ROW_LAST = 5;    // 最后一行数据
static const uint8_t CHAR_ADVANCE = 6;      // 每字符占 6 列（5 字形 + 1 间距）
static const uint8_t MAX_COLS = 21;         // 128 / CHAR_ADVANCE
static const uint8_t LABEL_COLS = 4;        // 标签宽度
static const uint8_t VALUE_COLS = MAX_COLS - LABEL_COLS;  // 17，数值列宽度

// ------------------------------------------------------------------ 右下角水印
// 四页的第 7 页（底部 8 像素）都不放数据，整行空闲，用来放署名。
// 用普通 5x7 字模，与页内数据同字号。
//
// LOGO_X 是右对齐算出来的，不是手调的：9 字符 × 6 列 = 54，
// 末字符实体占列 122..126、字间距占 127，正好贴右边缘。
// 若改动 LOGO_TEXT 的字符数，必须重算此值——
// tools/check_layout.py 会独立复算并报错。
static const char* const LOGO_TEXT = "SIUNGOLAI";
static const uint8_t ROW_LOGO = 7;   // 第 7 页，即屏幕最底部 8 像素
static const uint8_t LOGO_X = 74;    // 128 - (9 * 6) = 74

// 版本号，写在署名同一行的最左侧（第 7 页列 0 起）。
// 与水印共存的前提：水印固定占列 74..127（即列 12 之后），
// 所以左侧可用 12 列；本值 4 字符占列 0..3，中间留 8 列间隔。
// 改长到 12 字符以上就会撞上水印——tools/check_layout.py 会报错。
static const char* const VERSION_TEXT = "V1.0";

// ------------------------------------------------------------------ 字模方向自检
// 上电时按住按钮 >= 1.5s 才跑字模方向自检（Q20-C）：
// 驱动画两条相隔 14 行的 1x4 参照竖条，停留 6 秒后自动清屏。
// 两个时长都由调用方传入，不写死在驱动里；常态上电不跑，保证上电即显。
static const uint32_t FONT_DIR_HOLD_MS = 1500;

// ------------------------------------------------------------------ 复位计数（RTC）
// 用 RTC_NOINIT_ATTR 而非 RTC_DATA_ATTR：前者落在 .rtc_noinit（NOLOAD），
// 启动时不会从 flash 映像重新初始化，所以软复位 / 看门狗复位后值还在；
// 后者落在 .rtc.data，每次上电都会按 flash 里的初值覆盖，计数永远归零。
// 代价：真拔电后 RTC 慢速内存掉电即丢，且首次烧录后里面是垃圾值，
// 所以必须靠 magic 校验区分「垃圾」与「有效计数」。见 Q17-A。
static const uint32_t RTC_MAGIC = 0x53544131;  // "STA1"

}  // namespace config