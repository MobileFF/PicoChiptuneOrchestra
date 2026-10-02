// Standalone ST7735 TFT text-rendering test -- MASTER Pico only.
//
// Links the REAL driver (master/src/st7735.c + spi0_bus_lock.c -- the exact
// code that ships in master.uf2, not a reimplementation like
// tools/oled_test/'s own from-scratch SSD1306 driver), so a good result
// here specifically confirms st7735.c's text/font rendering path on real
// hardware. Complements tools/st7735_test/'s MicroPython script, which
// already confirmed raw init + solid-colour fills (and found the right
// MADCTL for this panel) with an independent implementation -- this test
// instead answers "does the actual shipped driver draw readable text
// correctly", isolated from the rest of the firmware (no SD card, no SPI
// slaves, no multicore, no vgm_player, no [player] display config --
// st7735_init() is called directly).
//
// Wiring (same as the real firmware -- docs/circuit.md section 1.1b):
//   VCC->3V3  GND->GND  SCK->GPIO18  SDA(MOSI)->GPIO19
//   CS->GPIO3  DC->GPIO4  RST->GPIO5  BL->3V3  MISO: leave unconnected
//
// How to use:
//   1. Flash st7735_text_test.uf2 onto the MASTER Pico (hold BOOTSEL, copy
//      the file, let it reboot).
//   2. Open the USB CDC serial port (baud rate doesn't matter for USB CDC).
//   3. Watch the panel AND the log together -- see README.md's decision guide.
//
// Build: see tools/st7735_text_test/README.md.

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "st7735.h"
#include "spi0_bus_lock.h"
#include "tft_pins.h"

int main(void) {
    stdio_init_all();
    sleep_ms(3000); // let a USB CDC terminal attach before the first print

    printf("\n=== ST7735 TFT text test (real master/src/st7735.c driver) ===\n");
    printf("SPI0  SCK=GPIO18  MOSI=GPIO19  CS=GPIO%u  DC=GPIO%u  RST=GPIO%u\n",
           TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);

    // st7735.c's cmd()/data()/show() unconditionally take this lock (see
    // spi0_bus_lock.h) -- nothing else uses SPI0 in this standalone test,
    // so it's never actually contended, but the mutex must still be
    // initialized before st7735_init() takes it for the first time.
    spi0_bus_lock_init();

    st7735_init(spi0, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);
    printf("st7735_init() done. It always \"succeeds\" -- this panel can't ACK "
           "back over SPI (see st7735_init()'s doc comment) -- so watch the "
           "panel itself, not this return value, to judge the result.\n");

    uint32_t frame = 0;
    for (;;) {
        st7735_clear();
        st7735_text(0, 0,  "ST7735 TEXT TEST");
        st7735_text(0, 1,  "master/src/st7735.c");
        st7735_text(0, 3,  "!\"#$%&'()*+,-./0123");
        st7735_text(0, 4,  "456789:;<=>?@ABCDEF");
        st7735_text(0, 5,  "GHIJKLMNOPQRSTUVWXY");
        st7735_text(0, 6,  "Zabcdefghijklmnopqr");
        st7735_text(0, 7,  "stuvwxyz{|}~");
        st7735_text(0, 9,  "If every row above is");
        st7735_text(0, 10, "sharp, aligned, and");
        st7735_text(0, 11, "not clipped/garbled,");
        st7735_text(0, 12, "the real font/text");
        st7735_text(0, 13, "path is confirmed OK.");

        // A changing counter proves the loop (and the full-framebuffer
        // push) keeps running, not just a one-shot static frame.
        char line[32];
        snprintf(line, sizeof(line), "frame %lu", (unsigned long)frame);
        st7735_text(0, 16, line);

        bool ok = st7735_show();
        printf("frame %lu shown (show()=%d)\n", (unsigned long)frame, (int)ok);
        frame++;
        sleep_ms(1000);
    }
}
