#include "rlcd.h"

#include "config.h"

namespace {

RlcdDisplay *g_lcd = nullptr;  // 全仓库只有一块屏，单实例足够

// 与官方例程 10_U8G2_Test / BSP factory / LVGL 例程三处同源（HARDWARE.md §11.7）
u8x8_display_info_t kDisplayInfo = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 20,
    /* post_reset_wait_ms = */ 50,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ cfg::kSpiClockHz,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 4,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ cfg::kTileWidth,
    /* tile_height = */ cfg::kTileHeight,
    /* default_x_offset = */ 0,
    /* flip_mode_x_offset = */ 0,
    /* pixel_width = */ 300,   // 竖屏口径，R1 旋转后逻辑宽变 400
    /* pixel_height = */ 400,  // 竖屏口径
};

// 4 列像素 -> 1 字节，每列占 2 bit。官方例程 ST7305_U8g2.cpp:156-161 原表。
const uint8_t kStLut[4][4] = {
    {0x00, 0x80, 0x40, 0xC0},
    {0x00, 0x20, 0x10, 0x30},
    {0x00, 0x08, 0x04, 0x0C},
    {0x00, 0x02, 0x01, 0x03},
};

}  // namespace

RlcdDisplay::RlcdDisplay(int sck, int mosi, int dc, int cs, int rst)
    : sck_(sck), mosi_(mosi), dc_(dc), cs_(cs), rst_(rst) {
  g_lcd = this;
}

// ---------------------------------------------------------------- 底层时序

void RlcdDisplay::cmd(uint8_t c) {
  digitalWrite(dc_, LOW);
  digitalWrite(cs_, LOW);
  spi_->transfer(c);
  digitalWrite(cs_, HIGH);
}

void RlcdDisplay::cmdData(uint8_t c, const uint8_t *data, size_t len) {
  digitalWrite(dc_, LOW);
  digitalWrite(cs_, LOW);
  spi_->transfer(c);
  if (len > 0) {
    digitalWrite(dc_, HIGH);
    spi_->transferBytes(const_cast<uint8_t *>(data), nullptr, len);
  }
  digitalWrite(cs_, HIGH);
}

void RlcdDisplay::resetPanel() {
  digitalWrite(rst_, HIGH);
  delay(50);
  digitalWrite(rst_, LOW);
  delay(20);
  digitalWrite(rst_, HIGH);
  delay(50);
}

// ---------------------------------------------------------------- u8x8 回调

uint8_t RlcdDisplay::byteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  (void)u8x8;
  (void)arg_int;
  (void)arg_ptr;
  // 字节传输由本类自己的 cmd/cmdData 直接完成，这里只对 SETUP_MEMORY 之类
  // 的握手消息应答成功。SPI 的 begin/beginTransaction 已在 begin() 里做好。
  switch (msg) {
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_START_TRANSFER:
    case U8X8_MSG_BYTE_END_TRANSFER:
    case U8X8_MSG_BYTE_SET_DC:
    case U8X8_MSG_BYTE_SEND:
      return 1;
    default:
      return 1;
  }
}

uint8_t RlcdDisplay::displayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  if (!g_lcd) {
    return 0;
  }

  switch (msg) {
    case U8X8_MSG_DISPLAY_SETUP_MEMORY:
      u8x8_d_helper_display_setup_memory(u8x8, &kDisplayInfo);
      return 1;

    case U8X8_MSG_DISPLAY_INIT:
      g_lcd->fullInit();
      return 1;

    case U8X8_MSG_DISPLAY_SET_CONTRAST:
      g_lcd->setContrast((uint8_t)arg_int);
      return 1;

    case U8X8_MSG_DISPLAY_DRAW_TILE: {
      auto *tile = reinterpret_cast<u8x8_tile_t *>(arg_ptr);
      const uint8_t cnt = tile->cnt;
      const uint8_t y_pos = tile->y_pos;
      const uint8_t x_pos = tile->x_pos;

      int first_col = x_pos * 8;
      int last_col = (x_pos + cnt) * 8 - 1;
      if (last_col >= 300) {
        last_col = 299;
      }

      const int addr_start = 0x12 + first_col / 12;
      const int addr_end = 0x12 + last_col / 12;
      const int send_start = (addr_start - 0x12) * 3;
      const int send_cnt = (addr_end - addr_start + 1) * 3;

      int addr_first_col = (addr_start - 0x12) * 12;
      int addr_last_col = (addr_end - 0x12) * 12 + 11;
      if (addr_last_col >= 300) {
        addr_last_col = 299;
      }

      // 回到该缓冲行的行首（tile_ptr 指向 x_pos 处）
      const uint8_t *row_base = tile->tile_ptr - (uint16_t)x_pos * 8U;

      // 列地址递减：0x3C 是反向基准
      const uint8_t col_bounds[2] = {(uint8_t)(0x3C - addr_end), (uint8_t)(0x3C - addr_start)};
      g_lcd->cmdData(0x2A, col_bounds, sizeof(col_bounds));

      const uint8_t row_bounds[2] = {(uint8_t)(y_pos * 4), (uint8_t)(y_pos * 4 + 3)};
      g_lcd->cmdData(0x2B, row_bounds, sizeof(row_bounds));

      uint8_t all_rows[300] = {0};
      for (int sr = 0; sr < 4; sr++) {
        const int shift = sr * 2;
        int idx = sr * send_cnt + (addr_first_col >> 2) - send_start;
        for (int col = addr_first_col; col <= addr_last_col; col += 4, idx++) {
          all_rows[idx] = kStLut[0][(row_base[col] >> shift) & 3] |
                          kStLut[1][(row_base[col + 1] >> shift) & 3] |
                          kStLut[2][(row_base[col + 2] >> shift) & 3] |
                          kStLut[3][(row_base[col + 3] >> shift) & 3];
        }
      }

      g_lcd->cmdData(0x2C, all_rows, (size_t)send_cnt * 4U);
      return 1;
    }

    default:
      return 0;
  }
}

// ---------------------------------------------------------------- 初始化

void RlcdDisplay::fullInit() {
  resetPanel();

  const uint8_t d6[] = {0x17, 0x02};
  const uint8_t d1[] = {0x01};
  const uint8_t c0[] = {0x11, 0x04};
  const uint8_t c1[] = {0x69, 0x69, 0x69, 0x69};
  const uint8_t c2[] = {0x19, 0x19, 0x19, 0x19};
  const uint8_t c4[] = {0x4B, 0x4B, 0x4B, 0x4B};
  const uint8_t d8[] = {0x80, 0xE9};
  const uint8_t b3[] = {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t b4[] = {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45};
  const uint8_t g_timing[] = {0x32, 0x03, 0x1F};
  const uint8_t b7[] = {0x13};
  const uint8_t b0[] = {0x64};
  const uint8_t c9[] = {0x00};
  const uint8_t m36[] = {0x48};
  const uint8_t m3a[] = {0x11};
  const uint8_t b9[] = {0x20};
  const uint8_t b8[] = {0x29};
  const uint8_t win_a[] = {0x12, 0x2A};  // 300 列
  const uint8_t win_b[] = {0x00, 0xC7};  // 400 行
  const uint8_t m35[] = {0x00};
  const uint8_t d0[] = {0xFF};

  cmdData(0xD6, d6, sizeof(d6));
  cmdData(0xD1, d1, sizeof(d1));
  cmdData(0xC0, c0, sizeof(c0));
  cmdData(0xC1, c1, sizeof(c1));
  cmdData(0xC2, c2, sizeof(c2));
  cmdData(0xC4, c4, sizeof(c4));
  cmdData(0xC5, c2, sizeof(c2));
  cmdData(0xD8, d8, sizeof(d8));
  // 0xB2 = 面板自刷新档率。v0.1.3 起由 setPanelHz() 一处说了算
  // （ADR-0019 决定 7：1 Hz / 8 Hz 两档），初始化仍然从 1 Hz 开场。
  setPanelHz(1);
  cmdData(0xB3, b3, sizeof(b3));
  cmdData(0xB4, b4, sizeof(b4));
  cmdData(0x62, g_timing, sizeof(g_timing));
  cmdData(0xB7, b7, sizeof(b7));
  cmdData(0xB0, b0, sizeof(b0));
  cmd(0x11);  // sleep out
  delay(120);
  cmdData(0xC9, c9, sizeof(c9));
  cmdData(0x36, m36, sizeof(m36));
  cmdData(0x3A, m3a, sizeof(m3a));
  cmdData(0xB9, b9, sizeof(b9));
  cmdData(0xB8, b8, sizeof(b8));
  cmd(0x21);  // 反显开（与官方例程一致；配 0x20 会发灰）
  cmdData(0x2A, win_a, sizeof(win_a));
  cmdData(0x2B, win_b, sizeof(win_b));
  cmdData(0x35, m35, sizeof(m35));
  cmdData(0xD0, d0, sizeof(d0));
  cmd(0x38);  // HPM
  cmd(0x29);  // display on
}

bool RlcdDisplay::begin(uint8_t tile_buf_height, const u8g2_cb_t *rotation) {
  pinMode(dc_, OUTPUT);
  pinMode(cs_, OUTPUT);
  pinMode(rst_, OUTPUT);
  digitalWrite(cs_, HIGH);
  digitalWrite(dc_, HIGH);
  digitalWrite(rst_, HIGH);

  spi_ = new SPIClass(HSPI);
  if (!spi_) {
    return false;
  }
  spi_->begin(sck_, -1, mosi_, -1);  // MISO 不用
  spi_->beginTransaction(SPISettings(cfg::kSpiClockHz, MSBFIRST, SPI_MODE0));

  uint8_t tbh = tile_buf_height == 0 ? cfg::kTileHeight : tile_buf_height;
  if (tbh > cfg::kTileHeight) {
    tbh = cfg::kTileHeight;
  }

  const size_t buf_sz = (size_t)cfg::kBufferRowBytes * tbh;
  buf_ = (uint8_t *)malloc(buf_sz);
  if (!buf_) {
    return false;
  }
  memset(buf_, 0, buf_sz);

  u8g2_t *u = u8g2_.getU8g2();
  u8x8_Setup(u8g2_GetU8x8(u), displayCb, u8x8_dummy_cb, byteCb, u8x8_dummy_cb);
  u8g2_SetupBuffer(u, buf_, tbh, u8g2_ll_hvline_vertical_top_lsb, rotation);
  u8g2_InitDisplay(u);
  u8g2_SetPowerSave(u, 0);
  u8g2_ClearBuffer(u);

  ready_ = true;
  return true;
}

// ---------------------------------------------------------------- 模式与刷新

void RlcdDisplay::setHpm() {
  cmd(0x38);
  lpm_ = false;
}

void RlcdDisplay::setLpm() {
  cmd(0x39);
  lpm_ = true;
}

void RlcdDisplay::setPanelHz(uint8_t hz) {
  // 0xB2 的两个有效值（ADR-0019 决定 7）：
  //   0x02 = 1 Hz —— u8g2 官方 ST7305 例程的初始化值，v0.1.2 及以前一直用它
  //   0x05 = 8 Hz —— 厂商 BSP factory 的值，只在动画窗口里开
  // 其它值一律按 1 Hz 处理：写一个手册没写明的值等于拿面板做实验，
  // 而 ADR-0019 已经把「切档代价」列成待测项，不该再多一个未知数。
  const uint8_t v = (hz >= 8) ? 0x05 : 0x02;
  cmdData(0xB2, &v, sizeof(v));
  panel_hz_ = (v == 0x05) ? 8 : 1;
}

void RlcdDisplay::setContrast(uint8_t level) {
  // 编码照抄 u8g2 官方驱动（u8x8_d_st7305.c:311-317）：命令 0x81 + 两个参数。
  // 与数据手册的对应关系尚未核对，M0 只用肉眼确认「有没有变化」。
  const uint8_t args[2] = {(uint8_t)(level << 2), (uint8_t)(level >> 6)};
  cmdData(0x81, args, sizeof(args));
}

uint32_t RlcdDisplay::benchRawSpi(size_t chunk, size_t total) {
  if (!ready_ || chunk == 0) {
    return 0;
  }
  if (chunk > 512) {
    chunk = 512;
  }
  uint8_t buf[512];
  memset(buf, 0, sizeof(buf));
  uint32_t t0 = micros();
  for (size_t sent = 0; sent < total; sent += chunk) {
    spi_->transferBytes(buf, nullptr, chunk);
  }
  return micros() - t0;
}

void RlcdDisplay::refreshAll() {
  if (!ready_) {
    return;
  }
  if (lpm_) {
    setHpm();
    delay(cfg::kHpmSettleMs);
  }
  u8g2_.sendBuffer();
}

void RlcdDisplay::refreshArea(int x, int y, int w, int h) {
  if (!ready_ || w <= 0 || h <= 0) {
    return;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > cfg::kLcdWidth) w = cfg::kLcdWidth - x;
  if (y + h > cfg::kLcdHeight) h = cfg::kLcdHeight - y;
  if (w <= 0 || h <= 0) {
    return;
  }

  if (lpm_) {
    setHpm();
    delay(cfg::kHpmSettleMs);
  }

  // u8g2 的坐标单位是 tile（8x8），向下取整起点、向上取整终点。
  const uint8_t tx = (uint8_t)(x / 8);
  const uint8_t ty = (uint8_t)(y / 8);
  const uint8_t tw = (uint8_t)((x + w + 7) / 8 - tx);
  const uint8_t th = (uint8_t)((y + h + 7) / 8 - ty);
  // 注意：u8g2_UpdateDisplayArea() 的旋转支持不完整，R1 下是否落点正确
  // 要在 M1 上板验（策划案 §7 第 5 项）。M0 只用 refreshAll()。
  u8g2_UpdateDisplayArea(u8g2_.getU8g2(), tx, ty, tw, th);
}
