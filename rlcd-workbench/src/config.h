// rlcd-workbench / 硬件常量集中处
//
// 所有引脚与几何尺寸只在这里定义一次。换板、换屏只改这个文件，
// 其他文件不出现裸数字。
//
// 来源：HARDWARE.md §11（官方资料离线归档核实，非本机实测）。
// 串口日志只用 ASCII，这条是仓库约定。

#pragma once

#include <stdint.h>

namespace cfg {

// ---- 显示屏（ST7305，4.2 寸全反射 RLCD）----
// 物理 300x400，实际按 400x300 横屏驱动。u8g2 的 ST7305 驱动是竖屏定义，
// 必须显式传 U8G2_R1 才交换成长边在 X 轴（详见 docs/adr/0001）。
constexpr int kLcdSck = 11;
constexpr int kLcdMosi = 12;
constexpr int kLcdDc = 5;
constexpr int kLcdCs = 40;
constexpr int kLcdRst = 41;
constexpr int kLcdTe = 6;  // 撕裂同步，本项目不用

constexpr int kLcdWidth = 400;   // 逻辑宽（横屏）
constexpr int kLcdHeight = 300;  // 逻辑高（横屏）

// u8g2 的 tile 几何：每 tile 8x8 像素
constexpr int kTileWidth = 38;   // 38*8 = 304 列（>300，末尾补空）
constexpr int kTileHeight = 50;  // 50*8 = 400 行
constexpr int kBufferRowBytes = kTileWidth * 8;  // 304
constexpr int kBufferBytes = kBufferRowBytes * kTileHeight;  // 15200

// SPI 时钟。官方例程用 24 MHz；GPIO 矩阵路径上限约 40 MHz，
// ST7305 写时序要求 >= 30 ns（约 33 MHz），24 MHz 留足余量。
constexpr uint32_t kSpiClockHz = 24000000;

// ---- 按键 ----
constexpr int kKeyBoot = 0;   // 低有效，上电 1 秒内的事件要丢弃（进下载模式）
constexpr int kKeyUser = 18;  // 低有效，内部上拉

// ---- I2C（RTC / 温湿度，M2 才用）----
constexpr int kI2cSda = 13;
constexpr int kI2cScl = 14;

// ---- 电池采样（M2 才用）----
constexpr int kBatAdc = 4;

// ---- 时序 ----
constexpr uint32_t kBootKeyIgnoreMs = 1000;  // 上电后丢弃 BOOT 事件的窗口
constexpr uint32_t kHpmSettleMs = 100;       // 切 HPM 后等画面稳定再写（自定值，待实测校正）

// ---- 空闲省电 ----
// 无按键、无数据变化超过这个时长就切 LPM（0x39），有交互立刻切回 HPM（0x38）。
constexpr uint32_t kIdleToLpmMs = 30000;  // docs/adr/0005
// 对比度 5 档，BOOT 长按循环。编码见 RlcdDisplay::setContrast。
constexpr uint8_t kContrastLevels[] = {40, 50, 60, 70, 80};
constexpr uint8_t kContrastCount = sizeof(kContrastLevels) / sizeof(kContrastLevels[0]);
constexpr uint16_t kContrastHintMs = 2000;  // 角落「对比度 3/5」显示时长

// ---- 界面几何（全部来自 docs/策划案.md §4.2 / §4.3 / §4.4 的实测字宽）----
constexpr int kHeaderH = 24;
constexpr int kFooterH = 22;
constexpr int kContentTop = kHeaderH + 1;                 // 25（页首下面一条分隔线）
constexpr int kContentBottom = kLcdHeight - kFooterH - 1;  // 277
constexpr int kSplitX = 200;                               // 两栏等宽，各 200 px
constexpr int kMarginL = 4;
constexpr int kMarginR = kLcdWidth - 4;  // 396

// ---- 日历（三行滚动窗口，docs/adr/0016）----
constexpr int kCalTop = 49;        // 第一行格子顶
constexpr int kCalRowH = 20;       // 格高
constexpr int kCalRowPitch = 21;   // 行距
constexpr int kCalCellW = 26;      // 格宽
constexpr int kCalColPitch = 27;   // 列距
constexpr int kCalRows = 3;
constexpr int kCalLeft = 204;      // 右栏 x

// ---- 待办（6 槽位）----
constexpr int kTodoSlots = 6;
constexpr int kTodoTop = 126;
constexpr int kTodoPitch = 25;

}  // namespace cfg
