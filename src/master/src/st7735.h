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

// Cover-art area: the TOP COVER_AREA_H rows, reserved for cover_image.c's
// once-per-folder image (see cover_image.h). The remaining rows (bottom 64px,
// 8 pages) are the status text area -- the same 8-page layout the OLED has
// always used, just shifted down here instead of starting at row 0 (see
// oled_ui.c's per-backend text_page_offset). st7735_clear() below only
// touches the text area; the cover area is solely st7735_cover_clear()'s and
// st7735_cover_blit()'s to manage, so it isn't repainted (and the image
// doesn't need to be redecoded) on every ~500ms redraw.
#define COVER_AREA_W ST7735_W
#define COVER_AREA_H (ST7735_H - 8 * 8) // 96px -- leaves exactly 8 pages (64px) for text

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

// Claims `cs_gpio`/`dc_gpio`/`rst_gpio` as plain GPIO outputs, pulses reset
// and sends the panel's init sequence. This panel doesn't have a way to ACK
// back over SPI (write-only wiring, MISO usually left unconnected), so
// unlike ssd1306_init() this always returns true -- the caller can't
// distinguish "panel present and working" from "nothing wired to those pins
// at all".
//
// `spi0_already_configured`: pass true when the SD card driver has already
// brought SPI0 up at the (identical) baud/format this project uses -- i.e.
// whenever main.c's f_mount("0:") ran before this (always, in the shipped
// firmware: see main.c's boot order). false re-runs spi_init()/
// spi_set_format()/the SCK+MOSI gpio_set_function() calls to bring SPI0 up
// from scratch (needed for a from-scratch build with no SD card at all --
// tools/st7735_text_test/ -- which has nothing else to rely on).
//
// Hit in the field (2026-10-02): passing false here even when SD was
// *already* initialized (this function's previous, unconditional behaviour)
// reproduced the SD card's `FR_DISK_ERR` bug from the very first directory
// listing of every single boot, deterministically -- not a race at all.
// pico-sdk's spi_init() calls reset_block_num()/unreset_block_num_wait_
// blocking() on the WHOLE SPI0 peripheral, which can glitch the now-shared
// SCK/MOSI lines right after the SD card's own last transaction (the ini
// file read moments earlier in main()) -- exactly the trigger for the
// "MMC/SDC keeps listening to SCLK/DI even once deselected" quirk
// documented in sd_spi.c's sd_spi_deselect() comment, wedging the card
// before its very first real use. The register VALUES spi_init() leaves
// behind are identical either way (same baud/format), so this was
// previously assumed a harmless no-op -- the RESET ITSELF, not the end
// state, was the actual hazard. See docs/design-notes.md for the full
// diagnostic trail (several SPI0-arbitration fixes tried first, none of
// which touched this because it isn't a timing/ordering bug).
bool st7735_init(spi_inst_t *spi, uint cs_gpio, uint dc_gpio, uint rst_gpio,
                  bool spi0_already_configured);

// Framebuffer edits (RAM only; call st7735_show() to push). Clears only the
// TEXT area (rows COVER_AREA_H..ST7735_H-1) -- see COVER_AREA_H's comment.
void st7735_clear(void);
// Draw NUL-terminated ASCII 0x20..0x7E at pixel column `x`, page row `page`
// (0..ST7735_PAGES-1, each page = 8px tall, matching ssd1306_text()).
// Characters past the right edge are clipped, not wrapped. Pages inside
// COVER_AREA_H are valid to pass but not how cover_image.c draws -- it uses
// st7735_cover_blit() directly, which works in raw pixel coordinates.
void st7735_text(uint8_t x, uint8_t page, const char *s);

// Clears the COVER area (rows 0..COVER_AREA_H-1) to background. Called by
// cover_image.c once per folder, either on its own (no image this folder)
// or right before st7735_cover_blit() calls (so letterboxed margins around
// a smaller/differently-shaped image aren't stale pixels from a previous
// folder's cover).
void st7735_cover_clear(void);

// Copies a w x h RGB565 (native-endian uint16_t, row-major, no padding)
// rectangle into the cover area's framebuffer at (x, y). Silently clips
// anything outside the cover area (0 <= x,y and x+w, y+h <= COVER_AREA_W/H)
// instead of writing out of bounds. `pixels` is read once, not retained.
void st7735_cover_blit(int x, int y, int w, int h, const uint16_t *pixels);

// Push the whole framebuffer (cover area + text area) to the panel. Always
// returns true (see st7735_init()'s note -- this panel can't report a
// failed write), kept bool-returning only so oled_ui.c's render() can call
// either backend the same way.
bool st7735_show(void);
