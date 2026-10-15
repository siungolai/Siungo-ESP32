/**
 * ssd1306_i2c.h -- SSD1306 128x64 I2C bare driver
 *
 * Copied verbatim from clock-oled/src/ssd1306_i2c.h, which itself came from
 * uptime-oled (both measured working on the same screen model). Screen model,
 * wiring, and drawing needs are identical here. Only the comment language
 * changed; no logic was touched.
 *
 * Why not use an off-the-shelf library:
 *   - Adafruit_SSD1306 ships a 1KB full-screen buffer + GFX abstraction
 *     layer: slow to compile, heavy dependencies;
 *   - this task only needs "draw text + a divider line", which the bare
 *     driver covers in about 30 commands, and the init sequence is written
 *     straight from the SSD1306 datasheet, fully under control.
 *
 * Framebuffer model: 128x64 is 8 pages; each page is 8 pixel rows x 128
 *   columns; one byte is one column of 8 vertical pixels, bit0 = the top
 *   row within the page. Full screen = 8 * 128 = 1024 bytes.
 */
#pragma once

#include <stdint.h>

// ------------------------------------------------------------------ constants
static const uint8_t OLED_WIDTH = 128;
static const uint8_t OLED_HEIGHT = 64;
static const uint8_t OLED_PAGES = OLED_HEIGHT / 8;  // 8 pages
static const uint16_t OLED_BUFFER_SIZE =
    static_cast<uint16_t>(OLED_WIDTH) * OLED_PAGES;  // 1024 bytes

// SSD1306 command codes (only the ones this driver uses)
enum Ssd1306Cmd : uint8_t {
  CMD_DISPLAY_OFF = 0xAE,
  CMD_DISPLAY_ON = 0xAF,
  CMD_SET_LOW_COLUMN = 0x00,        // low 4 bits of the column address
  CMD_SET_HIGH_COLUMN = 0x10,       // high 4 bits of the column address
  CMD_MEMORY_MODE = 0x20,           // 0x00 horizontal / 0x01 vertical / 0x02 page
  CMD_SET_CONTRAST = 0x81,
  CMD_CHARGE_PUMP = 0x8D,           // 0x14 = enable internal charge pump
  CMD_SET_START_LINE = 0x40,
  CMD_SEG_REMAP = 0xA0,             // |0x01 = horizontal flip
  CMD_SET_MULTIPLEX = 0xA8,
  CMD_DISPLAY_ALLON_RESUME = 0xA4,  // show framebuffer content
  CMD_NORMAL_DISPLAY = 0xA6,
  CMD_COM_SCAN_DEC = 0xC8,          // vertical flip
  CMD_SET_DISPLAY_OFFSET = 0xD3,
  CMD_SET_CLOCK_DIV = 0xD5,
  CMD_SET_PRECHARGE = 0xD9,
  CMD_SET_COM_PINS = 0xDA,
  CMD_SET_VCOM_DETECT = 0xDB,
  CMD_SET_PAGE = 0xB0,              // |page number (0..7)
  CMD_DEACTIVATE_SCROLL = 0x2E,
};

// ------------------------------------------------------------------ driver class
class Ssd1306I2c {
 public:
  // scl/sda: any GPIO on ESP32; addr: 7-bit address (0x3C or 0x3D)
  Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr = 0x3C,
             uint32_t hz = 400000);

  // Start I2C, probe the slave, run the init sequence.
  // false = the slave did not ACK.
  bool begin();

  bool ready() const { return ok_; }

  // ---- drawing (framebuffer only, no I2C) ----
  //
  // Vertical positioning comes in two equivalent spellings:
  //   taking a page (*Page variants) -- starts at the page's top edge, so
  //                                    line spacing is a multiple of 8;
  //   taking a y (*Y variants)      -- starts at any pixel row, spacing free.
  // Use the *Y variants when the layout must fill 64 rows without leaving
  // whole pages blank.
  void clear();
  void drawChar5x7(uint8_t x, uint8_t page, char c);
  // Returns the next usable x (including the inter-character gap), so mixed
  // Chinese/English text can continue from there.
  uint8_t drawText5x7(uint8_t x, uint8_t page, const char* s);
  uint8_t drawText5x7Y(uint8_t x, uint8_t y, const char* s);
  // 2x scale-up: 14 pixels tall, spans pages
  void drawText5x7Scaled2x(uint8_t x, uint8_t page, const char* s);
  void drawText5x7Scaled2xY(uint8_t x, uint8_t y, const char* s);
  void fillPageSpan(uint8_t x0, uint8_t x1, uint8_t page, uint8_t pattern);

  // ---- Chinese (16x16 bitmap, 16 columns wide, 16 rows tall) ----
  // A glyph missing from the table draws a box placeholder, making a
  // forgotten character easy to spot.
  void drawCnChar(uint8_t x, uint8_t page, uint32_t codepoint);
  void drawCnCharY(uint8_t x, uint8_t y, uint32_t codepoint);
  // Draw a UTF-8 Chinese string; returns the x where it ended.
  uint8_t drawTextCn(uint8_t x, uint8_t page, const char* s);
  uint8_t drawTextCnY(uint8_t x, uint8_t y, const char* s);
  // Pixel width of a UTF-8 Chinese string (16 columns per glyph).
  static uint8_t cnTextWidth(const char* s);

  // Generic bitmap blit: OR a "one byte per column" bitmap onto the
  // framebuffer at any y. rowsHeight is 8 or 16 (with 16, each column is
  // 2 bytes, low byte first).
  void drawBits(uint8_t x, uint8_t y, const uint8_t* cols, uint8_t n,
                uint8_t rowsHeight);

  // Push the 1024-byte framebuffer to the screen (8 pages, chunked).
  void flush();

  // Glyph direction self-check: lights two 1x4 vertical bars 14 rows apart,
  // clears the screen after 6 s. Called automatically by begin(). How to
  // read the result: see src/font_dir.h.
  void fontDirProbe();

  // ---- low level ----
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
