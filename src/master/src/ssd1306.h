// ssd1306.h -- minimal I2C SSD1306 128x64 OLED driver for the master's
// status display. Monochrome, page-addressed, whole-framebuffer push. Text
// is drawn with a built-in 5x7 font on an 8px page grid (21 chars x 8 rows).
//
// Everything here runs on core1 only (see oled_ui.c) -- the driver itself
// keeps no lock and must not be called from both cores.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hardware/i2c.h"

#define SSD1306_W 128
#define SSD1306_H 64
#define SSD1306_PAGES (SSD1306_H / 8)   // 8
#define SSD1306_COLS_PER_LINE (SSD1306_W / 6) // 21 chars with the 5x7 font + 1px gap

// Sends the power-on command sequence over `i2c` to slave address `addr`
// (0x3C on most bare modules). Returns false if the panel does not ACK --
// the caller should then treat the display as absent and carry on (the
// player works fine without it).
bool ssd1306_init(i2c_inst_t *i2c, uint8_t addr);

// Framebuffer edits (RAM only; call ssd1306_show() to push).
void ssd1306_clear(void);
// Draw NUL-terminated ASCII 0x20..0x7E at pixel column `x`, page row
// `page` (0..7). Characters past the right edge are clipped, not wrapped.
void ssd1306_text(uint8_t x, uint8_t page, const char *s);

// Push the whole 1KB framebuffer to the panel (~23ms at 400kHz). Returns
// false if the I2C write did not complete (panel gone / bus wedged).
bool ssd1306_show(void);
