#include "tft_big.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "font5x7.h"
#include "spi0_bus_lock.h"
#include "tft_panel.h"
#include "tft_pins.h"

// --- init sequences -------------------------------------------------------
//
// Each entry is {cmd, n, n data bytes}. cmd 0xFE is a delay of n*10 ms
// (no data); 0xFF ends the table.
//
// This is the minimal SWRESET/SLPOUT/PIXFMT/MADCTL/DISPON sequence, not the
// longer gamma/power-sequence tables many vendor examples include -- cross-
// checked against two other local RP2040/RP2350 projects with real ST7796
// (and, for one of them, ILI9341 too) hardware running on this exact
// sequence shape: neither sends a command-set-2 unlock (0xF0) or an
// inversion command (0x20/0x21) at all, relying on the panel's power-on
// reset defaults for both, and neither sends any gamma/power tuning (0xC0-
// 0xC5/0xE0/0xE1/0xE8/0xED/0xCB/etc) -- panel defaults were fine. One of the
// two DOES send the 0xF0 unlock/relock and a Display Function Control
// (0xB6, same 0x80 0x02 0x3B data used below) around an otherwise-identical
// core sequence; both are kept here since the unlock is harmless when the
// panel doesn't need it.
//
// What's still genuinely unverified for THIS project: this firmware drives
// both panels in PORTRAIT (ILI9341 240x320, ST7796 320x480), while both
// reference projects run LANDSCAPE panels (480x320 / 320x240) -- their
// confirmed MADCTL (0x28 = MV|BGR) is a landscape value that doesn't apply
// here. *_MADCTL below is tunable (like st7735.h's own ST7735_MADCTL) --
// if rows/columns are swapped or the image is mirrored, try the other
// MY/MX/BGR combinations (0x08/0x48/0x88/0xC8) with MV left clear. If
// colours are swapped (not shifted) but geometry is otherwise correct,
// that's BGR vs RGB (bit 0x08), not MADCTL's MV or an inversion command --
// a mistake easy to misdiagnose as "needs INVON", per one of those projects'
// own notes.
#ifndef ILI9341_MADCTL
#define ILI9341_MADCTL 0x48 // portrait, BGR -- see this block's comment above
#endif
#ifndef ST7796_MADCTL
#define ST7796_MADCTL 0x48  // portrait, BGR -- see this block's comment above
#endif
// 0 = send neither INVON nor INVOFF (both reference projects' default, and
// what ships here); 1 = send INVON (0x21) if colours look washed out/
// negative; 2 = send INVOFF (0x20) for a panel that inverts the other way.
#ifndef ST7796_INVERT
#define ST7796_INVERT 0
#endif

static const uint8_t ILI9341_INIT[] = {
    0x01, 0, 0xFE, 15, // SWRESET, wait 150ms
    0x11, 0, 0xFE, 26, // SLPOUT, wait 260ms
    0x3A, 1, 0x55,     // PIXFMT/COLMOD: 16 bits/pixel
    0x36, 1, ILI9341_MADCTL,
    0x29, 0, 0xFE, 2,  // DISPON, wait 20ms
    0xFF,
};

static const uint8_t ST7796_INIT[] = {
    0x01, 0, 0xFE, 15, // SWRESET, wait 150ms
    0x11, 0, 0xFE, 26, // SLPOUT, wait 260ms
    0xF0, 1, 0xC3,     // unlock command set 2 (harmless if not needed)
    0xF0, 1, 0x96,
    0x3A, 1, 0x55,     // COLMOD: 16 bits/pixel
    0x36, 1, ST7796_MADCTL,
    0xB6, 3, 0x80, 0x02, 0x3B, // Display Function Control
    0xF0, 1, 0x3C,     // re-lock command set 2
    0xF0, 1, 0x69,
    0x29, 0, 0xFE, 2,  // DISPON, wait 20ms
    0xFF,
};

// --- state -----------------------------------------------------------------

static spi_inst_t *s_spi;
static uint s_cs, s_dc;

#define TEXT_MAX_ITEMS 24
#define TEXT_MAX_CHARS (TFT_MAX_W / TFT_BIG_CELL_W)

typedef struct {
    uint16_t x;
    uint8_t page;
    char s[TEXT_MAX_CHARS + 1];
} text_item_t;

static text_item_t s_items[TEXT_MAX_ITEMS];
static uint8_t s_nitems;

// One text band (full panel width x 16 rows), big-endian RGB565 as sent on the
// wire. Also used for the cover-art row scratch -- these never overlap in
// time (both run under spi0_bus_lock, and each fills then sends its own).
static uint8_t s_band[TFT_MAX_W * TFT_BIG_PAGE_H * 2];
static uint8_t s_row[TFT_MAX_W * 2];

// --- low-level SPI (callers hold spi0_bus_lock()) -----------------------------

static void bus_begin(void) {
    spi0_bus_lock();
    spi_set_baudrate(s_spi, TFT_PANEL_SPI_HZ);
}

static void bus_end(void) {
    spi_set_baudrate(s_spi, SPI0_BUS_BAUD_HZ);
    spi0_bus_unlock();
}

static void put_cmd(uint8_t c) {
    gpio_put(s_cs, 0);
    gpio_put(s_dc, 0);
    spi_write_blocking(s_spi, &c, 1);
    gpio_put(s_cs, 1);
}

static void put_data(const uint8_t *b, size_t n) {
    gpio_put(s_cs, 0);
    gpio_put(s_dc, 1);
    spi_write_blocking(s_spi, b, n);
    gpio_put(s_cs, 1);
}

// Every caller below ALSO sends its own put_cmd(0x2C) right after calling
// this, so RAMWR is sent TWICE per window (once here, once more by the
// caller) -- redundant on paper (this already ends on RAMWR). Briefly
// removed as dead code (2026-10-08), which coincided with a real ILI9341
// board going from "visible but sheared" to "nothing at all" -- restored to
// isolate that variable. Turned out NOT to be the actual cause (root cause
// was a marginal/intermittent physical connection -- pin-header-into-socket,
// not even loose jumper wires -- re-seating it fixed both panels; see
// design-notes.md's 2026-10-08 entries). Left in anyway: both ILI9341 and
// ST7796 are hardware-confirmed working WITH this redundancy present, so
// there's no reason to re-introduce the risk of removing it again for a
// purely cosmetic cleanup.
static void set_window(int x0, int y0, int x1, int y1) {
    uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    put_cmd(0x2A); put_data(ca, 4);
    put_cmd(0x2B); put_data(ra, 4);
    put_cmd(0x2C); // RAMWR: the data that follows fills the window
}

static void run_seq(const uint8_t *seq) {
    while (*seq != 0xFF) {
        uint8_t c = seq[0], n = seq[1];
        if (c == 0xFE) {
            busy_wait_us((uint32_t)n * 10000u); // see oled_ui.c's core1 notes: no sleep_*()
            seq += 2;
            continue;
        }
        bus_begin();
        put_cmd(c);
        if (n) put_data(seq + 2, n);
        bus_end();
        seq += 2 + n;
    }
}

// --- init ---------------------------------------------------------------------

bool tft_big_init(spi_inst_t *spi, uint cs_gpio, uint dc_gpio, uint rst_gpio,
                  bool spi0_already_configured) {
    s_spi = spi;
    s_cs = cs_gpio;
    s_dc = dc_gpio;

    gpio_init(cs_gpio); gpio_set_dir(cs_gpio, GPIO_OUT); gpio_put(cs_gpio, 1);
    gpio_init(dc_gpio); gpio_set_dir(dc_gpio, GPIO_OUT); gpio_put(dc_gpio, 1);
    gpio_init(rst_gpio); gpio_set_dir(rst_gpio, GPIO_OUT);

    // Same reasoning as st7735_init()'s spi0_already_configured block: a
    // from-scratch SPI0 bring-up is only correct when the SD card driver
    // hasn't done it already.
    if (!spi0_already_configured) {
        spi0_bus_lock();
        spi_init(s_spi, SPI0_BUS_BAUD_HZ);
        spi_set_format(s_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        gpio_set_function(TFT_SPI0_SCK_GPIO, GPIO_FUNC_SPI);
        gpio_set_function(TFT_SPI0_MOSI_GPIO, GPIO_FUNC_SPI);
        spi0_bus_unlock();
    }

    gpio_put(rst_gpio, 1); busy_wait_us(10000);
    gpio_put(rst_gpio, 0); busy_wait_us(10000);
    gpio_put(rst_gpio, 1); busy_wait_us(120000);

    run_seq(player_config_tft_panel() == PLAYER_TFT_ST7796 ? ST7796_INIT : ILI9341_INIT);

#if ST7796_INVERT != 0
    if (player_config_tft_panel() == PLAYER_TFT_ST7796) {
        bus_begin();
        put_cmd(ST7796_INVERT == 1 ? 0x21 : 0x20);
        bus_end();
    }
#endif

    // Clear the whole panel once -- a freshly powered panel shows noise.
    const tft_geom_t *g = tft_panel_geom();
    memset(s_row, 0, g->w * 2);
    bus_begin();
    set_window(0, 0, g->w - 1, g->h - 1);
    put_cmd(0x2C); // intentional redundancy -- see set_window()'s comment
    for (int y = 0; y < g->h; y++) put_data(s_row, g->w * 2);
    bus_end();
    return true; // like st7735: this panel is write-only, so it can't report failure
}

// --- text area --------------------------------------------------------------

void tft_big_clear(void) { s_nitems = 0; }

void tft_big_text(uint8_t x, uint8_t page, const char *s) {
    if (s_nitems >= TEXT_MAX_ITEMS) return;
    text_item_t *it = &s_items[s_nitems];
    it->x = x;
    it->page = page;
    size_t n = strlen(s);
    if (n > TEXT_MAX_CHARS) n = TEXT_MAX_CHARS;
    memcpy(it->s, s, n);
    it->s[n] = '\0';
    s_nitems++;
}

// Writes one ON pixel (white) at band-relative (px, py), bounds-checked.
static void band_pixel_on(int w, int px, int py) {
    if (px < 0 || px >= w || py < 0 || py >= TFT_BIG_PAGE_H) return;
    size_t i = ((size_t)py * w + px) * 2;
    s_band[i] = 0xFF;
    s_band[i + 1] = 0xFF;
}

static void band_draw_item(const text_item_t *it, int w) {
    int x = it->x;
    for (const char *c = it->s; *c; c++) {
        if (x >= w) break;
        unsigned char ch = (unsigned char)*c;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *glyph = FONT5x7[ch - 0x20];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(glyph[col] & (1u << row))) continue;
                for (int dy = 0; dy < TFT_BIG_FONT_SCALE; dy++)
                    for (int dx = 0; dx < TFT_BIG_FONT_SCALE; dx++)
                        band_pixel_on(w, x + col * TFT_BIG_FONT_SCALE + dx,
                                      row * TFT_BIG_FONT_SCALE + dy);
            }
        }
        x += TFT_BIG_CELL_W;
    }
}

bool tft_big_show(void) {
    const tft_geom_t *g = tft_panel_geom();
    int pages = (g->h - g->cover_h) / TFT_BIG_PAGE_H;
    for (int p = 0; p < pages; p++) {
        memset(s_band, 0, (size_t)g->w * TFT_BIG_PAGE_H * 2);
        for (int i = 0; i < s_nitems; i++)
            if (s_items[i].page == p) band_draw_item(&s_items[i], g->w);

        int y0 = g->cover_h + p * TFT_BIG_PAGE_H;
        bus_begin();
        set_window(0, y0, g->w - 1, y0 + TFT_BIG_PAGE_H - 1);
        put_cmd(0x2C); // intentional redundancy -- see set_window()'s comment
        put_data(s_band, (size_t)g->w * TFT_BIG_PAGE_H * 2);
        bus_end();
    }
    return true;
}

// --- cover-art area ------------------------------------------------------------

void tft_big_cover_clear(void) {
    const tft_geom_t *g = tft_panel_geom();
    memset(s_row, 0, g->w * 2);
    bus_begin();
    set_window(0, 0, g->w - 1, g->cover_h - 1);
    put_cmd(0x2C); // intentional redundancy -- see set_window()'s comment
    for (int y = 0; y < g->cover_h; y++) put_data(s_row, g->w * 2);
    bus_end();
}

void tft_big_cover_blit(int x, int y, int w, int h, const uint16_t *pixels) {
    const tft_geom_t *g = tft_panel_geom();
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= g->cover_h) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w > g->w) ? g->w : x + w; // exclusive
        if (x1 <= x0) continue;
        for (int dx = x0; dx < x1; dx++) {
            uint16_t px = pixels[row * w + (dx - x)];
            s_row[(dx - x0) * 2] = (uint8_t)(px >> 8);
            s_row[(dx - x0) * 2 + 1] = (uint8_t)px;
        }
        bus_begin();
        set_window(x0, dy, x1 - 1, dy);
        put_cmd(0x2C); // intentional redundancy -- see set_window()'s comment
        put_data(s_row, (size_t)(x1 - x0) * 2);
        bus_end();
    }
}
