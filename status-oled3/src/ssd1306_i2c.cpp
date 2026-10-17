/**
 * ssd1306_i2c.cpp -- SSD1306 128x64 I2C bare driver implementation
 *
 * Copied verbatim from clock-oled/src/ssd1306_i2c.cpp (measured working on
 * real hardware). Comment language only; no logic was touched.
 *
 * I2C frame format (datasheet chapter 8):
 *   control byte 0x00 = commands follow (Co=0, D/C#=0)
 *   control byte 0x40 = display data follows (Co=0, D/C#=1)
 */
#include "ssd1306_i2c.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "font5x7.h"
#include "font_cn.h"

namespace {

// Payload ceiling per I2C transaction. The ESP32 Arduino Wire buffer is
// about 128 bytes, so a 1024-byte frame is split into several chunks.
constexpr size_t kChunkPayload = 64;

// How long the glyph-direction self-check pattern stays on screen (ms).
constexpr uint32_t kFontDirProbeMs = 6000;

// Find this codepoint in the Chinese glyph table and return its 32-byte
// bitmap; nullptr when not found. The table holds only a couple dozen
// glyphs, so a linear scan beats building a hash table.
const uint8_t* cnGlyph(uint32_t codepoint) {
  for (uint16_t i = 0; i < kFontCnCount; ++i) {
    const char* p = kFontCnChars + i * 3;  // a Chinese glyph is 3 bytes in UTF-8
    const uint32_t cp = (static_cast<uint32_t>(p[0] & 0x0F) << 12) |
                        (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
                        (static_cast<uint32_t>(p[2]) & 0x3F);
    if (cp == codepoint) {
      return kFontCn[i];
    }
  }
  return nullptr;
}

// Pull one codepoint out of a UTF-8 string and advance the pointer.
// Only 1-3 byte sequences are handled (ASCII and Chinese both fall in this
// range), which is enough for this program.
uint32_t utf8Next(const char*& s) {
  const uint8_t c = static_cast<uint8_t>(*s);
  if (c < 0x80) {           // ASCII
    ++s;
    return c;
  }
  if ((c & 0xE0) == 0xC0) { // 2 bytes
    const uint32_t cp = ((c & 0x1F) << 6) |
                        (static_cast<uint8_t>(s[1]) & 0x3F);
    s += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0) { // 3 bytes (Chinese)
    const uint32_t cp = ((c & 0x0F) << 12) |
                        ((static_cast<uint8_t>(s[1]) & 0x3F) << 6) |
                        (static_cast<uint32_t>(s[2]) & 0x3F);
    s += 3;
    return cp;
  }
  ++s;  // 4 bytes or longer: not used by this program, just skip it
  return 0;
}

}  // namespace

Ssd1306I2c::Ssd1306I2c(uint8_t scl, uint8_t sda, uint8_t addr, uint32_t hz)
    : scl_(scl), sda_(sda), addr_(addr), hz_(hz) {}

void Ssd1306I2c::cmd(uint8_t c) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // control byte: command
  Wire.write(c);
  Wire.endTransmission();
}

void Ssd1306I2c::cmd2(uint8_t c, uint8_t arg) {
  Wire.beginTransmission(addr_);
  Wire.write(0x00);  // control byte: command
  Wire.write(c);
  Wire.write(arg);
  Wire.endTransmission();
}

bool Ssd1306I2c::probe() {
  Wire.beginTransmission(addr_);
  return Wire.endTransmission() == 0;
}

void Ssd1306I2c::setCursor(uint8_t page, uint8_t col) {
  // In page addressing mode, position the page first, then the column:
  // low 4 bits of the column + high 4 bits of the column.
  cmd(static_cast<uint8_t>(CMD_SET_PAGE | (page & 0x07)));
  cmd(static_cast<uint8_t>(CMD_SET_LOW_COLUMN | (col & 0x0F)));
  cmd(static_cast<uint8_t>(CMD_SET_HIGH_COLUMN | ((col >> 4) & 0x0F)));
}

bool Ssd1306I2c::begin() {
  // ESP32 allows arbitrary GPIO for I2C; begin first, then probe.
  Wire.begin(sda_, scl_, hz_);
  // 50 ms matches config::I2C_TIMEOUT_MS: a stuck bus must never block
  // forever. Note the timeout applies to the whole transaction, and there is
  // no separate screen-level "skip when absent" logic here -- that lives in
  // screen.cpp (HARDWARE.md section 6.2), because on this project one mux
  // stands behind three screens.
  Wire.setTimeOut(config::I2C_TIMEOUT_MS);

  ok_ = probe();
  if (!ok_) {
    return false;  // no ACK: wiring / address / pull-up issue, report to caller
  }

  // ---- power-on init sequence (SSD1306 datasheet recommended values) ----
  cmd(CMD_DISPLAY_OFF);        // 0xAE: display off first, avoid garbage on screen
  cmd2(CMD_SET_CLOCK_DIV, 0x80);   // 0xD5 0x80: divide=1, oscillator step 8
  cmd2(CMD_SET_MULTIPLEX, OLED_HEIGHT - 1);  // 0xA8 0x3F: 64-row multiplex
  cmd2(CMD_SET_DISPLAY_OFFSET, 0x00);        // 0xD3 0x00: no vertical offset
  cmd(CMD_SET_START_LINE | 0x00);            // 0x40: display start line 0
  cmd2(CMD_CHARGE_PUMP, 0x14);               // 0xD9 0x14: enable charge pump (required at 3.3V)
  cmd2(CMD_MEMORY_MODE, 0x02);               // 0x20 0x02: page addressing mode (no wrap across pages)
  cmd(CMD_SEG_REMAP | 0x01);                 // 0xA1: column 127 -> SEG0 (horizontal mirror baseline)
  cmd(CMD_COM_SCAN_DEC);                     // 0xC8: reversed COM scan (vertical flip baseline)
  cmd2(CMD_SET_COM_PINS, 0x12);              // 0xDA 0x12: 128x64 alternative COM pin config
  cmd2(CMD_SET_CONTRAST, 0xCF);              // 0x81 0xCF: brightness
  cmd2(CMD_SET_PRECHARGE, 0xF1);             // 0xD9 0xF1: precharge period
  cmd2(CMD_SET_VCOM_DETECT, 0x40);           // 0xDB 0x40: VCOMH level
  cmd(CMD_DISPLAY_ALLON_RESUME);             // 0xA4: show framebuffer content
  cmd(CMD_NORMAL_DISPLAY);                   // 0xA6: normal (not inverted)
  cmd(CMD_DEACTIVATE_SCROLL);                // 0x2E: scrolling off
  cmd(CMD_DISPLAY_ON);                       // 0xAF: display on

  clear();
  flush();
  fontDirProbe();
  return true;
}

// ------------------------------------------------------------------ glyph direction self-check

void Ssd1306I2c::fontDirProbe() {
  // Draw only vertical bars; do not touch glyphs. Reason (see font_dir.h):
  // if a banner used 16x16 glyphs to check glyph direction, that would mean
  // "using possibly mirrored ink to write the very words that check
  // direction", and the check itself becomes unreliable.
  // Two same-length (4-row) bars, 14 rows apart vertically; on screen the
  // answer "which bar is farther from the vertical center" is evidence from
  // the screen itself, independent of glyph orientation.
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
  vbar(2, 27, 4);    // left edge, upper
  vbar(120, 35, 4);  // right edge, lower
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
  // Do not flush an absent screen: every endTransmission blocks up to the
  // timeout, and 8 pages x 2 transactions can drag loop() for almost a
  // second, wrecking the 1 s refresh cadence. Skip when probe failed, so the
  // main loop's timing precision is unaffected.
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
      Wire.write(0x40);  // control byte: display data
      for (uint8_t i = 0; i < n; ++i) {
        Wire.write(p[i]);
      }
      Wire.endTransmission();
      p += n;
      remaining -= n;
    }
  }
}

// ------------------------------------------------------------------ drawing
//
// Vertical positioning: every drawing function takes a pixel row y, not a
// page number.
//   The early version used page numbers, because SSD1306 memory is organized
//   in pages and flush also walks pages.
//   But a glyph is often 14-16 pixels tall and must span pages; if it could
//   only start on a page boundary, line spacing would be a multiple of 8,
//   and a 64-row screen would be forced to leave whole pages blank.
//   With pixel rows, line spacing can be any value.
//
// Internally everything goes through drawBits: OR a glyph's columns onto the
// framebuffer at an arbitrary y; whichever pages a column spans gets its bit
// set. All glyph functions share this one bit-twiddling site, instead of
// each re-implementing the page-crossing math.

void Ssd1306I2c::drawBits(uint8_t x, uint8_t y, const uint8_t* cols, uint8_t n,
                          uint8_t rowsHeight) {
  // cols is a "one byte per column" bitmap, n columns, each rowsHeight rows
  // (8 or 16). With 16 rows, low byte first then high byte, same layout as
  // the Chinese glyph table.
  const uint8_t bytesPerCol = static_cast<uint8_t>(rowsHeight / 8);
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t col = x + i;
    if (col >= OLED_WIDTH) {
      break;
    }
    for (uint8_t b = 0; b < rowsHeight; ++b) {
      const uint8_t byte = cols[i * bytesPerCol + b / 8];
      if ((byte & (1u << (b % 8))) == 0) {
        continue;  // off pixels are not written, keep what is in the buffer
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
  // Column 6 is the inter-character gap: cleared explicitly, so a shorter
  // string does not leave remnants of a previously longer one.
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
    // Per-pixel vertical positioning: on pixels set the matching bit, off
    // pixels do not overwrite what is already there.
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
    // 2x scale-up: source column i -> target columns 2i, 2i+1; source bit b
    // -> target pixel rows 2b, 2b+1.
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

void Ssd1306I2c::invertPageSpan(uint8_t x0, uint8_t x1, uint8_t page) {
  if (page >= OLED_PAGES || x0 > x1) {
    return;
  }
  uint8_t* row = buf_ + static_cast<size_t>(page) * OLED_WIDTH;
  for (uint8_t x = x0; x <= x1 && x < OLED_WIDTH; ++x) {
    row[x] = static_cast<uint8_t>(row[x] ^ 0xFF);
  }
}

// ------------------------------------------------------------------ Chinese
//
// A Chinese glyph is 16 pixels tall and must be able to land on any pixel
// row (see "vertical positioning" at the top of this file).
// Glyph layout: 2 bytes per column, low byte is the upper half (rows 0..7),
// high byte is the lower half (rows 8..15).

void Ssd1306I2c::drawCnCharY(uint8_t x, uint8_t y, uint32_t codepoint) {
  // Rebuild the glyph as a 16-column x 16-row bitmap, then hand it to the
  // generic drawBits. Per-pixel math inline would also work, but it would
  // duplicate the page-crossing bit math; a 16-byte temp array is small
  // enough that the compiler keeps it in registers/stack.
  uint8_t cols[16];
  const uint8_t* g = cnGlyph(codepoint);
  if (g == nullptr) {
    // Glyph not in the table: draw a hollow box as placeholder.
    // Better than drawing nothing -- blank looks like a rendering bug,
    // a box reads immediately as "missing glyph".
    for (uint8_t i = 0; i < 16; ++i) {
      const bool edge = (i == 0 || i == 15);
      cols[i] = edge ? 0xFF : 0x81;
    }
    drawBits(x, y, cols, 16, 8);         // upper half
    drawBits(x, y + 8, cols, 16, 8);     // lower half, same box
    return;
  }

  // Upper half: first 16 bytes of the glyph, one byte per column
  drawBits(x, y, g, 16, 8);
  // Lower half: last 16 bytes of the glyph
  drawBits(x, static_cast<uint8_t>(y + 8), g + 16, 16, 8);
}

void Ssd1306I2c::drawCnChar(uint8_t x, uint8_t page, uint32_t codepoint) {
  drawCnCharY(x, static_cast<uint8_t>(page * 8), codepoint);
}

uint8_t Ssd1306I2c::drawTextCnY(uint8_t x, uint8_t y, const char* s) {
  uint8_t cursor = x;
  while (*s != '\0') {
    if (cursor + 16 > OLED_WIDTH) {
      break;  // the whole glyph does not fit on the right; stop drawing
    }
    const uint32_t cp = utf8Next(s);
    drawCnCharY(cursor, y, cp);
    cursor += 16;  // a Chinese glyph is fixed 16 columns wide, no extra gap
                  // (the bitmap carries its own margin)
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
