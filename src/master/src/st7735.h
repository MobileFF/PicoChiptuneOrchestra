// st7735.h -- minimal SPI ST7735 128x160 TFT driver for the master's status
// display, an alternative to ssd1306.h's I2C OLED (see oled_ui.c and
// [player] display in player_config.h). RGB565, whole-framebuffer push, same
// built-in 5x7 font (font5x7.h) and 8px-page text API as ssd1306.h so
// oled_ui.c's rendering logic (render()/draw_wrapped()/draw_chips()) works
// against either backend unchanged.
//
// Shares the master's physical SPI0 bus with the SD card (see
// docs/circuit.md) -- every transaction here must be wrapped in
// spi0_bus_lock()/spi0_bus_unlock() (st7735.c does this internally; callers
// don't need to). Everything here runs on core1 only (see oled_ui.c) -- the
// driver itself keeps no lock of its own and must not be called from both
// cores.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hardware/spi.h"

// Most common cheap breakout size. A 128x128 module works too (rows past
// 128 just never get anything drawn to them) -- only ST7735_H would need
// changing for that, everything else in this driver is size-derived.
#define ST7735_W 128
#define ST7735_H 160
#define ST7735_COLS_PER_LINE (ST7735_W / 6) // 21 chars with the 5x7 font + 1px gap, same as SSD1306
#define ST7735_PAGES (ST7735_H / 8)         // 20 -- far more than the 8 the OLED has

// Panel-variant tuning knobs. Cheap ST7735 breakouts come in a few
// "tab colour" variants that disagree on a small RAM offset and on the
// panel's native RGB vs BGR pixel order; get a wiring-correct but
// wrong-coloured or edge-shifted image and adjust these two, not the init
// sequence itself:
//   - ST7735_XSTART/YSTART: a 1-2px black band on one edge (or the image
//     wrapping around) means the visible area doesn't start at the RAM's
//     (0,0) -- try 2,1 (common "green tab") or 2,3.
//   - ST7735_MADCTL: swapped red/blue (a red status text look blue, say)
//     means the panel is the other RGB/BGR order -- toggle bit 0x08.
#ifndef ST7735_XSTART
#define ST7735_XSTART 0
#endif
#ifndef ST7735_YSTART
#define ST7735_YSTART 0
#endif
#ifndef ST7735_MADCTL
// MX|MY row/col order + RGB (not BGR -- 0xC8 showed red/blue swapped on the
// actual panel this project was built against, confirmed via
// tools/st7735_test/ 2026-10-01). A different module may need 0xC8 instead;
// see this block's top comment.
#define ST7735_MADCTL 0xC0
#endif

// SPI0's baud rate is deliberately left exactly matching the SD card's
// (hw_config.c's 20 MHz) -- see spi0_bus_lock.h and st7735.c's top comment
// for why this driver never calls spi_set_baudrate().
#define ST7735_SPI_BAUD_HZ (20 * 1000 * 1000)

// Brings up `spi` (spi0 in practice -- the shared bus) at ST7735_SPI_BAUD_HZ
// if it isn't already, claims `cs_gpio`/`dc_gpio`/`rst_gpio` as plain GPIO
// outputs, pulses reset and sends the panel's init sequence. This panel
// doesn't have a way to ACK back over SPI (write-only wiring, MISO usually
// left unconnected), so unlike ssd1306_init() this always returns true --
// the caller can't distinguish "panel present and working" from "nothing
// wired to those pins at all".
bool st7735_init(spi_inst_t *spi, uint cs_gpio, uint dc_gpio, uint rst_gpio);

// Framebuffer edits (RAM only; call st7735_show() to push).
void st7735_clear(void);
// Draw NUL-terminated ASCII 0x20..0x7E at pixel column `x`, page row `page`
// (0..ST7735_PAGES-1, each page = 8px tall, matching ssd1306_text()).
// Characters past the right edge are clipped, not wrapped.
void st7735_text(uint8_t x, uint8_t page, const char *s);

// Push the whole framebuffer to the panel. Always returns true (see
// st7735_init()'s note -- this panel can't report a failed write), kept
// bool-returning only so oled_ui.c's render() can call either backend the
// same way.
bool st7735_show(void);
