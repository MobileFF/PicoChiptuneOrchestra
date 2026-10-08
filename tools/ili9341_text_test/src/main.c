// Standalone ILI9341 TFT text-rendering test -- MASTER Pico only.
//
// Links the REAL driver (master/src/tft_big.c + spi0_bus_lock.c -- the exact
// code that ships in master.uf2 for [player] tft_panel = ili9341, not a
// reimplementation), so a good result here specifically confirms tft_big.c's
// init + text path on real hardware, isolated from the rest of the firmware
// (no SD card, no SPI slaves, no multicore, no vgm_player, no [player]
// config parsing -- tft_big_init() is called directly). See
// tools/st7735_text_test/ for the same idea applied to the ST7735 driver.
//
// player_config_tft_panel()/tft_panel_geom() are stubbed below instead of
// linking the real player_config.c/tft_panel.c -- this test only ever wants
// PLAYER_TFT_ILI9341, and the real player_config.c drags in FatFs/SD-card
// code this test has no use for.
//
// IMPORTANT, if comparing this test's picture against tools/st7735_text_test's:
// tft_big_text()'s page 0 is NOT the physical top of the panel -- it's the
// top of the TEXT area, which starts BELOW the reserved cover-art area
// (160 of this panel's 320 rows -- see tft_panel_geom()'s stub below and
// tft_big.h's comment on this). st7735_text_test's st7735_text(0, 0, ...),
// by contrast, draws at the literal top-left of its panel (that offsetting
// is applied externally, by oled_ui.c, for the real firmware's ST7735
// backend). So this test's output will show the top ~half of the screen
// filled with a solid colour (below) and the text block starting partway
// down -- that is the expected layout, not a misconfiguration.
//
// Wiring (same as the real firmware -- docs/circuit.md section 1.1b):
//   VCC->3V3  GND->GND  SCK->GPIO18  SDA(MOSI)->GPIO19
//   CS->GPIO3  DC->GPIO4  RST->GPIO5  BL->3V3 (or per-module BL pin)
//   MISO: leave unconnected
//
// How to use:
//   1. Flash ili9341_text_test.uf2 onto the MASTER Pico (hold BOOTSEL, copy
//      the file, let it reboot).
//   2. Open the USB CDC serial port (baud rate doesn't matter for USB CDC).
//   3. Watch the panel, the Pico's own onboard LED, AND the log together --
//      see README.md's decision guide.
//
// Build: see tools/ili9341_text_test/README.md.

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "tft_big.h"
#include "tft_panel.h"
#include "spi0_bus_lock.h"
#include "tft_pins.h"
#include "player_config.h"

player_tft_panel_t player_config_tft_panel(void) { return PLAYER_TFT_ILI9341; }

const tft_geom_t *tft_panel_geom(void) {
    static const tft_geom_t g = {240, 320, 160}; // matches tft_panel.c's own table
    return &g;
}

// Pico's OWN onboard LED (GPIO25 on a genuine Pico) -- NOT the panel's
// backlight (this project ties the panel's BL straight to 3V3, always on,
// so it can't be toggled from firmware to prove anything). This project's
// panel has no ACK/busy line at all, so a hung or brownout-reset-looping
// MCU looks IDENTICAL on the wire to a panel that's simply not responding:
// spi_write_blocking() finishes once clocked out whether or not anything is
// listening. A peer project (RP2040, ILI9341/ST7796 hardware-confirmed)
// flagged that a panel's backlight current draw can brown out a marginal
// supply -- which would freeze or reset-loop the MCU, not just leave the
// panel dark -- so this heartbeat is independent proof the MCU itself kept
// running the whole time, which a dark panel alone can't tell you.
#define LED_PIN 25

static void led_blink(int times, uint32_t ms) {
    for (int i = 0; i < times; i++) {
        gpio_put(LED_PIN, 1); sleep_ms(ms);
        gpio_put(LED_PIN, 0); sleep_ms(ms);
    }
}

int main(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    stdio_init_all();
    // 10s (2026-10-08, was 3s): long enough to plug in, open a terminal, and
    // start a capture command (e.g. `screen`/`picocom`/`minicom -C log.txt`)
    // before the first print -- 3s wasn't enough time to do that by hand.
    sleep_ms(10000);
    led_blink(1, 200); // "main() reached, stdio up" -- before touching SPI0/RST at all

    printf("\n=== ILI9341 TFT text test (real master/src/tft_big.c driver) ===\n");
    printf("SPI0  SCK=GPIO18  MOSI=GPIO19  CS=GPIO%u  DC=GPIO%u  RST=GPIO%u  "
           "TFT_PANEL_SPI_HZ=%d\n", TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO,
           (int)TFT_PANEL_SPI_HZ);
    printf("Onboard LED (GPIO25): 1 blink just happened (main() alive); 2 more "
           "blinks right after tft_big_init() returns; then it heartbeats once "
           "a second forever in the main loop. If it stops at any point, the "
           "MCU itself hung/reset there -- note exactly which blink was last.\n");

    // tft_big.c's put_cmd()/put_data()/show() unconditionally take this lock
    // (see spi0_bus_lock.h) -- nothing else uses SPI0 in this standalone
    // test, so it's never actually contended, but the mutex must still be
    // initialized before tft_big_init() takes it for the first time.
    spi0_bus_lock_init();

    tft_big_init(spi0, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO, false); // no SD card here -- bring SPI0 up from scratch
    led_blink(2, 150); // "tft_big_init() returned" -- proves the whole init sequence didn't hang
    printf("tft_big_init() done. It always \"succeeds\" -- this panel can't ACK "
           "back over SPI (same as st7735_init(), see its doc comment) -- so "
           "watch the panel itself, not this return value, to judge the result.\n");

    // Y-RULER (2026-10-08): a real board reported the cover-area fill below
    // showing only briefly at boot, then getting overwritten by the TEXT
    // pages -- which tft_big_show() never addresses (every text-page window
    // write targets y >= cover_h, never below it; re-checked, this is not a
    // software bug in tft_big.c). That points at the PANEL not honouring the
    // row (RASET) address we send -- e.g. wrapping rows >= some smaller
    // value back to 0 -- rather than a layout bug. So instead of a solid
    // fill + arbitrary text, draw a directly-readable ruler: the cover area
    // (rows 0-159) gets 10 bands of 16px each in a distinct, nameable
    // colour (one per "page slot"), and the text area (rows 160-319) gets
    // each page's OWN intended absolute Y printed as its label. If the
    // panel is addressing rows correctly, "Y=160" should appear
    // immediately below the last colour band (white, rows 144-159), and
    // "Y=304" at the very bottom. If instead "Y=160" (etc.) appears
    // OVERLAPPING/overwriting a colour band, that band's row range is
    // where this panel's addressing actually wraps -- read off which color
    // band gets overwritten first to find the real usable height.
    static const uint16_t RULER_COLOR[10] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, // red, orange, yellow, green, cyan
        0x001F, 0xF81F, 0xFFFF, 0x8410, 0xFC18, // blue, magenta, white, gray, pink
    };
    static const char *RULER_NAME[10] = {
        "red", "orange", "yellow", "green", "cyan",
        "blue", "magenta", "white", "gray", "pink",
    };
    {
        const tft_geom_t *g = tft_panel_geom();
        static uint16_t row[TFT_MAX_W];
        printf("Cover-area colour ruler (rows 0-%u, 16px bands):\n", (unsigned)g->cover_h - 1);
        for (int band = 0; band * 16 < g->cover_h; band++) {
            for (int x = 0; x < g->w; x++) row[x] = RULER_COLOR[band % 10];
            printf("  rows %3d-%3d: %s\n", band * 16, band * 16 + 15, RULER_NAME[band % 10]);
            for (int dy = 0; dy < 16 && band * 16 + dy < g->cover_h; dy++)
                tft_big_cover_blit(0, band * 16 + dy, g->w, 1, row);
        }
        printf("If the text pages below (labelled with their own absolute Y) "
               "overwrite one of these colour bands instead of appearing "
               "cleanly below all of them, that tells you where this panel's "
               "real row addressing wraps.\n");
    }

    uint32_t frame = 0;
    bool led_on = false;
    for (;;) {
        tft_big_clear();
        for (int p = 0; p < 10; p++) {
            char label[32];
            int y0 = 160 + p * 16; // must match tft_big_show()'s own formula
            snprintf(label, sizeof(label), "PAGE%d Y=%d f%lu", p, y0, (unsigned long)frame);
            tft_big_text(0, p, label);
        }

        bool ok = tft_big_show();
        led_on = !led_on; // steady 1Hz heartbeat -- see the LED_PIN comment above
        gpio_put(LED_PIN, led_on);
        printf("frame %lu shown (show()=%d)\n", (unsigned long)frame, (int)ok);
        frame++;
        sleep_ms(1000);
    }
}
