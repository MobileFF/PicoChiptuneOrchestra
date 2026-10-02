#include "st7735.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "font5x7.h"
#include "spi0_bus_lock.h"

// --- state -----------------------------------------------------------------

static spi_inst_t *s_spi;
static uint s_cs_gpio, s_dc_gpio;
// 2 bytes/pixel, RGB565 -- see st7735.h's top comment for the RAM cost
// (~40KB) of keeping this as a whole framebuffer rather than drawing
// per-glyph windows.
static uint16_t s_fb[ST7735_W * ST7735_H];

#define COLOR_BG 0x0000 // black
#define COLOR_FG 0xFFFF // white

// SPI0's physical SCK/MOSI pins -- must match hw_config.c's spi_t.sck_gpio/
// mosi_gpio (the SD card's own wiring), since this is the same shared bus
// (see spi0_bus_lock.h). Not passed into st7735_init() as parameters: in
// this project they're fixed by the board wiring, same as TFT_CS_GPIO etc.
// in tft_pins.h, and nothing outside this file needs to know them.
#define TFT_SPI0_SCK_GPIO  18
#define TFT_SPI0_MOSI_GPIO 19

// --- low-level SPI -----------------------------------------------------------
//
// This panel is write-only (MISO is normally left unwired), so there is no
// equivalent of ssd1306.c's ACK-based presence check or i2c_write_timeout_us
// -- a byte sent to nothing wired at all "succeeds" exactly like a byte sent
// to a working panel. See st7735_init()'s doc comment.
//
// Every call here holds spi0_bus_lock() for the SD card's sake (see
// spi0_bus_lock.h) -- SPI0's baud/format is left exactly as the SD driver
// configured it (or as st7735_init() configures it, if TFT-only and SD
// mount happens to run later) rather than switched per-transaction, since
// neither side re-asserts it before every one of ITS OWN transactions either
// (see ST7735_SPI_BAUD_HZ's comment in st7735.h).

static void cmd(uint8_t c) {
    spi0_bus_lock();
    gpio_put(s_cs_gpio, 0);
    gpio_put(s_dc_gpio, 0); // command
    spi_write_blocking(s_spi, &c, 1);
    gpio_put(s_cs_gpio, 1);
    spi0_bus_unlock();
}

static void data(const uint8_t *buf, size_t len) {
    spi0_bus_lock();
    gpio_put(s_cs_gpio, 0);
    gpio_put(s_dc_gpio, 1); // data
    spi_write_blocking(s_spi, buf, len);
    gpio_put(s_cs_gpio, 1);
    spi0_bus_unlock();
}

static void data1(uint8_t b) { data(&b, 1); }

static void cmd_data(uint8_t c, const uint8_t *buf, size_t len) {
    cmd(c);
    if (len) data(buf, len);
}

// --- init --------------------------------------------------------------------

// ST7735 command names, spelled out where used below (this project's other
// drivers -- ssd1306.c -- inline raw command bytes with a comment instead of
// naming every one; the ST7735 sequence is long enough that names read
// better here).
#define ST_SWRESET 0x01
#define ST_SLPOUT  0x11
#define ST_FRMCTR1 0xB1
#define ST_FRMCTR2 0xB2
#define ST_FRMCTR3 0xB3
#define ST_INVCTR  0xB4
#define ST_PWCTR1  0xC0
#define ST_PWCTR2  0xC1
#define ST_PWCTR3  0xC2
#define ST_PWCTR4  0xC3
#define ST_PWCTR5  0xC4
#define ST_VMCTR1  0xC5
#define ST_INVOFF  0x20
#define ST_MADCTL  0x36
#define ST_COLMOD  0x3A
#define ST_CASET   0x2A
#define ST_RASET   0x2B
#define ST_RAMWR   0x2C
#define ST_GMCTRP1 0xE0
#define ST_GMCTRN1 0xE1
#define ST_NORON   0x13
#define ST_DISPON  0x29

bool st7735_init(spi_inst_t *spi, uint cs_gpio, uint dc_gpio, uint rst_gpio) {
    s_spi = spi;
    s_cs_gpio = cs_gpio;
    s_dc_gpio = dc_gpio;

    gpio_init(cs_gpio); gpio_set_dir(cs_gpio, GPIO_OUT); gpio_put(cs_gpio, 1);
    gpio_init(dc_gpio); gpio_set_dir(dc_gpio, GPIO_OUT); gpio_put(dc_gpio, 1);
    gpio_init(rst_gpio); gpio_set_dir(rst_gpio, GPIO_OUT);

    // spi_init() is safe to call even if the SD card driver already brought
    // SPI0 up first (main.c orders [player] display's oled_ui_init() after
    // SD mount+config load) -- same baud/format either way, see
    // ST7735_SPI_BAUD_HZ's comment, so re-asserting it here is a harmless
    // no-op in that case and self-sufficient if SD mount failed instead.
    //
    // spi_init()/spi_set_format() only configure the SPI0 PERIPHERAL --
    // they do NOT route SCK/MOSI's GPIO pins to it. In the integrated
    // firmware this went unnoticed because the SD card driver (FatFs_SPI/
    // sd_driver/spi.c's gpio_set_function() calls, run via sd_init_driver()
    // before oled_ui_init() -- see main.c's boot order) had always already
    // done it first. A from-scratch build with no SD card at all
    // (tools/st7735_text_test/) has nothing to rely on, so the panel never
    // saw a clock or any data -- every command below "succeeded" (this
    // panel can't ACK either way) while nothing reached the wire. Doing it
    // here too makes this function correct on its own regardless of
    // whether anything else has touched SPI0 first.
    spi0_bus_lock();
    spi_init(s_spi, ST7735_SPI_BAUD_HZ);
    spi_set_format(s_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(TFT_SPI0_SCK_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(TFT_SPI0_MOSI_GPIO, GPIO_FUNC_SPI);
    spi0_bus_unlock();

    // Hardware reset: the datasheet wants RST held low >=10us then high, and
    // >=120ms before the first command after that (SWRESET below needs the
    // same 120ms settle, so this covers both). busy_wait_us, not sleep_ms:
    // this runs on core1 (see oled_ui.c's core1 notes -- sleep_*() there
    // depends on core0 servicing the default alarm pool's timer IRQ, which
    // can stall for a long time during SD-heavy boot).
    gpio_put(rst_gpio, 1); busy_wait_us(10000);
    gpio_put(rst_gpio, 0); busy_wait_us(10000);
    gpio_put(rst_gpio, 1); busy_wait_us(120000);

    cmd(ST_SWRESET); busy_wait_us(150000);
    cmd(ST_SLPOUT);  busy_wait_us(255000);

    cmd_data(ST_FRMCTR1, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3);
    cmd_data(ST_FRMCTR2, (const uint8_t[]){0x01, 0x2C, 0x2D}, 3);
    cmd_data(ST_FRMCTR3, (const uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6);
    cmd_data(ST_INVCTR,  (const uint8_t[]){0x07}, 1);
    cmd_data(ST_PWCTR1,  (const uint8_t[]){0xA2, 0x02, 0x84}, 3);
    cmd_data(ST_PWCTR2,  (const uint8_t[]){0xC5}, 1);
    cmd_data(ST_PWCTR3,  (const uint8_t[]){0x0A, 0x00}, 2);
    cmd_data(ST_PWCTR4,  (const uint8_t[]){0x8A, 0x2A}, 2);
    cmd_data(ST_PWCTR5,  (const uint8_t[]){0x8A, 0xEE}, 2);
    cmd_data(ST_VMCTR1,  (const uint8_t[]){0x0E}, 1);
    cmd(ST_INVOFF);
    cmd_data(ST_MADCTL, (const uint8_t[]){ST7735_MADCTL}, 1);
    cmd_data(ST_COLMOD, (const uint8_t[]){0x05}, 1); // 16 bits/pixel (RGB565)

    // Standard gamma tables (public, used near-verbatim by most ST7735
    // libraries) -- purely a contrast/colour-accuracy tweak, the panel works
    // without these too.
    cmd_data(ST_GMCTRP1, (const uint8_t[]){0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,
                                            0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10}, 16);
    cmd_data(ST_GMCTRN1, (const uint8_t[]){0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                                            0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10}, 16);

    cmd(ST_NORON);  busy_wait_us(10000);
    cmd(ST_DISPON); busy_wait_us(100000);

    st7735_clear();
    st7735_show();
    return true; // see st7735_init()'s header comment -- can't actually know
}

// --- framebuffer -------------------------------------------------------------

void st7735_clear(void) {
    for (size_t i = 0; i < ST7735_W * ST7735_H; i++) s_fb[i] = COLOR_BG;
}

void st7735_text(uint8_t x, uint8_t page, const char *s) {
    if (page >= ST7735_PAGES) return;
    uint16_t y0 = (uint16_t)page * 8;
    for (; *s; s++) {
        if (x + 6 > ST7735_W) break;
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = FONT5x7[c - 0x20];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                uint16_t color = (bits & (1u << row)) ? COLOR_FG : COLOR_BG;
                s_fb[(y0 + row) * ST7735_W + (x + col)] = color;
            }
        }
        // 6th column + the glyph's own 8th row: inter-character/line gap,
        // always background (mirrors ssd1306_text()'s `row[x+5] = 0x00`).
        for (int row = 0; row < 8; row++) s_fb[(y0 + row) * ST7735_W + (x + 5)] = COLOR_BG;
        for (int col = 0; col < 5; col++) s_fb[(y0 + 7) * ST7735_W + (x + col)] = COLOR_BG;
        x += 6;
    }
}

// --- push --------------------------------------------------------------------

bool st7735_show(void) {
    uint8_t caset[4] = {0, (uint8_t)ST7735_XSTART, 0, (uint8_t)(ST7735_XSTART + ST7735_W - 1)};
    uint8_t raset[4] = {0, (uint8_t)ST7735_YSTART, 0, (uint8_t)(ST7735_YSTART + ST7735_H - 1)};
    cmd_data(ST_CASET, caset, 4);
    cmd_data(ST_RASET, raset, 4);

    // RAMWR's data phase is one continuous transaction for the whole
    // framebuffer (like ssd1306_show()'s single 0x40-tagged write) -- held
    // across the whole push rather than released row-by-row, so a
    // concurrent SD access can't tear a single frame in half on the wire.
    cmd(ST_RAMWR);
    spi0_bus_lock();
    gpio_put(s_cs_gpio, 0);
    gpio_put(s_dc_gpio, 1);
    // Framebuffer is native uint16_t (host/ARM little-endian); the panel
    // wants big-endian RGB565 on the wire, so byte-swap into a scratch line
    // at a time instead of the whole ~40KB framebuffer -- static, not on
    // core1's 2KB stack (see main.c's own core0-stack notes; core1's is the
    // same size).
    static uint8_t line[ST7735_W * 2];
    for (int y = 0; y < ST7735_H; y++) {
        for (int x = 0; x < ST7735_W; x++) {
            uint16_t px = s_fb[y * ST7735_W + x];
            line[x * 2 + 0] = (uint8_t)(px >> 8);
            line[x * 2 + 1] = (uint8_t)(px & 0xFF);
        }
        spi_write_blocking(s_spi, line, sizeof(line));
    }
    gpio_put(s_cs_gpio, 1);
    spi0_bus_unlock();
    return true; // see st7735_init()'s header comment
}
