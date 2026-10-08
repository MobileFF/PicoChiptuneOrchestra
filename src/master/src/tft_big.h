// tft_big.h -- ILI9341 (240x320) and ST7796 (320x480) SPI TFT driver for
// [player] display = tft with [player] tft_panel = ili9341|st7796.
//
// Unlike st7735.c, there is NO full-screen RAM framebuffer: at 240x320 or
// 320x480 RGB565 that's 150KB / 300KB, and the RP2040 has 264KB of SRAM total
// (the firmware's own BSS is already ~147KB). Instead:
//   - text is recorded by tft_big_text() and drawn at tft_big_show() one
//     16px-tall text band at a time, each band pushed to its own window;
//   - the cover-art area is written straight to the panel by
//     tft_big_cover_blit() (cover_image.c already decodes row-by-row, so
//     nothing needs buffering) and never touched by text redraws.
// Text is the 5x7 font at 2x scale (12x16 px per character cell).
//
// Every panel transaction holds spi0_bus_lock() for the whole window write,
// and drops SPI0 to TFT_PANEL_SPI_HZ while it does (the SD card driver expects
// SPI0 at SPI0_BUS_BAUD_HZ, so that's restored before unlocking).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hardware/spi.h"
#include "spi0_bus_lock.h" // SPI0_BUS_BAUD_HZ -- see TFT_PANEL_SPI_HZ below

#define TFT_BIG_FONT_SCALE 2
#define TFT_BIG_CELL_W (6 * TFT_BIG_FONT_SCALE)  // 12 px per character
#define TFT_BIG_PAGE_H (8 * TFT_BIG_FONT_SCALE)  // 16 px per text row

// The datasheet write-cycle rating (~66ns, ~15 MHz) is conservative: two
// other local RP2040/RP2350 projects report these controllers hardware-
// stable well past it while sharing the same SPI bus with an SD card
// (ST7796 at 40 MHz and even 62.5 MHz, ILI9341 at 26 MHz). Left matching
// SPI0_BUS_BAUD_HZ (the SD card's own rate) here instead of raising it
// further: that keeps bus_begin()/bus_end() a no-op baud change until
// there's a reason (a slow/long-fallthrough-wired panel) to lower it, and
// avoids tuning a 2nd, independent rate this project hasn't hardware-tested
// at all. Raise it (a build-time override) once this project's own panel is
// confirmed stable at a higher rate -- and see tools/ili9341_text_test's
// 2026-10-08 notes if lowering it instead, to rule out signal integrity on
// a specific board's wiring (long/breadboard wires, no series resistors).
#ifndef TFT_PANEL_SPI_HZ
#define TFT_PANEL_SPI_HZ SPI0_BUS_BAUD_HZ
#endif

// Initialize the panel selected by [player] tft_panel. Same
// spi0_already_configured contract as st7735_init() -- see st7735.h.
bool tft_big_init(spi_inst_t *spi, uint cs_gpio, uint dc_gpio, uint rst_gpio,
                  bool spi0_already_configured);

// Text area API, same CALL SHAPE as st7735.h's (x in pixels, page = a text
// row), but NOT the same coordinate origin: page here always counts 16px
// rows from the top of the TEXT area, i.e. tft_big_show() adds cover_h
// itself -- page 0 lands just below the cover-art area, never at the
// physical top of the panel. st7735_text()'s page, by contrast, is a raw
// framebuffer row from the panel's physical top -- oled_ui.c is the one that
// adds st7735's cover-area offset externally (BACKEND_ST7735's
// text_page_offset). That's why BACKEND_ILI9341/ST7796 set
// text_page_offset = 0 in oled_ui.c (this driver already baked the offset
// in) -- but it also means a standalone test that calls tft_big_text(0, 0, ...)
// will NOT show text at the physical top of the screen the way
// tools/st7735_text_test's st7735_text(0, 0, ...) does; the top cover_h
// pixels are reserved and start out black. See tools/ili9341_text_test/'s
// own comment for why its on-screen result looks "shifted" down vs. that
// other test if you don't expect this.
void tft_big_clear(void);
void tft_big_text(uint8_t x, uint8_t page, const char *s);
bool tft_big_show(void);

void tft_big_cover_clear(void);
void tft_big_cover_blit(int x, int y, int w, int h, const uint16_t *pixels);
