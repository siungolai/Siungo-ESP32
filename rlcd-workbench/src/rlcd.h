// rlcd-workbench / ST7305 显示封装
//
// 为什么不用 u8g2 自带的 U8G2_ST7305_300X400_* 构造器：
//   - 官方驱动只面向竖屏 300x400，DRAW_TILE 里把列窗口硬编码成 0x2A 0x01 0x2A，
//     配 U8G2_R1 拿横屏 400x300 时几何会错位（HARDWARE.md §11.7）。
//   - 官方驱动有两套 gamma，抄错画面发灰。
//   - 官方封装（厂商例程 10_U8G2_Test 的 ST7305_U8g2）能出正确画面，但把
//     u8x8 的 cad_cb 和 gpio_and_delay_cb 都设成 u8x8_dummy_cb，于是
//     sendF() 必然失败、u8g2_SetContrast() 走 display_cb 也被 default 分支
//     吃掉 —— 模式命令与对比度全都没有通道（docs/adr/0004）。
//
// 本封装的做法：抄官方例程的 DRAW_TILE 与 fullInit（gamma 逐字节照抄），
// 另外补上两条官方缺口 —— 一个真正能拉低 DC 的命令通道，和对比度。
//
// 刷新模型（docs/adr/0004）：
//   全屏刷新 refreshAll()   → u8g2->sendBuffer()
//   局部刷新 refreshArea()  → u8g2_UpdateDisplayArea()，要求全缓冲模式
//   空闲 setLpm() / 交互 setHpm()，切 HPM 后要等一会儿再写，否则画面延迟约 1 秒

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

class RlcdDisplay {
 public:
  RlcdDisplay(int sck, int mosi, int dc, int cs, int rst);

  // tile_buf_height = 0 表示全缓冲（15200 字节）。局部刷新必须是全缓冲。
  // 旋转默认 U8G2_R1：u8g2 的 ST7305 是竖屏定义，R1 才换成长边在 X 轴。
  bool begin(uint8_t tile_buf_height = 0, const u8g2_cb_t *rotation = U8G2_R1);

  U8G2 *g() { return ready_ ? &u8g2_ : nullptr; }
  bool ready() const { return ready_; }

  // ---- 命令通道（绕开 u8g2 的 CAD 层）----
  void cmd(uint8_t c);
  void cmdData(uint8_t c, const uint8_t *data, size_t len);

  // ---- 模式 ----
  void setHpm();  // 0x38 面板高频自刷新
  void setLpm();  // 0x39 面板低频自刷新
  // 对比度。编码照抄 u8g2 官方驱动 u8x8_d_st7305.c:311-317（0x81 + 两个参数）。
  // 语义未与数据手册核对，M0 用肉眼确认有无可见变化。
  void setContrast(uint8_t level);

  // 面板自刷新档率（0xB2）。这是「帧率」唯一的硬件旋钮（docs/adr/0019 决定 7）：
  //   1 Hz = 0x02（u8g2 官方例程值，v0.1.2 及以前一直用它）
  //   8 Hz = 0x05（厂商 BSP factory 值）
  // 本函数只写寄存器，**不动** HPM/LPM，也不自带等待：切档之后要不要立刻
  // setHpm()、要不要 delay(kHpmSettleMs)，由调用方决定。切档本身会不会花屏
  // 是 ADR-0019 的 1 号实测项（未观测），所以这里保持最小动作。
  void setPanelHz(uint8_t hz);
  uint8_t panelHz() const { return panel_hz_; }

  // ---- 刷新 ----
  void refreshAll();
  // 逻辑像素坐标；内部换算成 tile 坐标，向下取整、向上取整到 tile 边界。
  void refreshArea(int x, int y, int w, int h);

  // 裸 SPI 吞吐测量（M0 用来判断 16.1 ms/帧是「SPI 时钟慢」还是「每次调用的固定开销」）。
  // 只发数据不发命令，会把 RAM 指针之后的内容写花，调用方负责之后重画一帧。
  uint32_t benchRawSpi(size_t chunk, size_t total);

 private:
  static uint8_t byteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
  static uint8_t displayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
  void resetPanel();
  void fullInit();

  int sck_, mosi_, dc_, cs_, rst_;
  SPIClass *spi_ = nullptr;
  U8G2 u8g2_;
  uint8_t *buf_ = nullptr;
  bool ready_ = false;
  bool lpm_ = false;
  uint8_t panel_hz_ = 1;  // 0xB2 的当前档位（1 或 8），只用于调用方比对
};
