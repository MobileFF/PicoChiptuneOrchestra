// Minimal SPI bring-up test -- MASTER side. No VGM parsing, no SD card, no
// chip protocol, no multicore: just repeatedly sends an easy-to-recognize,
// per-frame-varying 3-byte pattern and logs what it sends over USB CDC /
// UART. Pairs with ../../slave.
//
// Two send modes, selected at build time:
//   (default)     -- one CS low/write/high pulse PER BYTE + a gap. This is
//                    what src/master/src/slave_bus.c does normally.
//   -DBRINGUP_BURST -- hold CS low across all 3 bytes (one pulse). ~8x
//                    faster; on the original RP2040<->RP2040 breadboard this
//                    dropped bytes 2/3. Use this build to check whether a
//                    given slave board (e.g. a Pico 2 YM2151 slave) can
//                    actually clock a continuous burst -- if RECV matches
//                    SEND with BURST, slave_bus.c can use `gap = 0` for it.
//
// CS GPIO defaults to 20 (the YM2151 slave's CS in the production wiring);
// override with -DBRINGUP_CS=<n> to match whatever CS the board under test
// is wired to.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define PIN_SCK 10
#define PIN_MOSI 11
#ifndef BRINGUP_CS
#define BRINGUP_CS 20
#endif
#define PIN_CS BRINGUP_CS
#define BAUD_HZ (4 * 1000 * 1000)

int main(void) {
    stdio_init_all();
    sleep_ms(3000); // give a USB CDC terminal time to attach before we start printing
#ifdef BRINGUP_BURST
    const char *mode = "BURST (CS held across 3 bytes)";
#else
    const char *mode = "per-byte CS pulse + 50us gap";
#endif
#ifdef BRINGUP_MODE1
    const char *spimode = "SPI mode 1 (CPHA=1)";
#else
    const char *spimode = "SPI mode 0 (CPHA=0)";
#endif
    printf("\n=== SPI bring-up TEST MASTER (SCK=GPIO%d MOSI=GPIO%d CS=GPIO%d, %d Hz, %s, %s) ===\n",
           PIN_SCK, PIN_MOSI, PIN_CS, BAUD_HZ, mode, spimode);

    spi_init(spi1, BAUD_HZ);
#ifdef BRINGUP_MODE1
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
#else
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
#endif
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    uint8_t counter = 0;
    for (;;) {
        // High nibble = byte POSITION (1/2/3), low nibble = shared counter --
        // so the receiver can tell which position a value came from and
        // whether it's actually varying frame to frame.
        uint8_t buf[3] = {
            (uint8_t)(0x10 | (counter & 0x0F)),
            (uint8_t)(0x20 | (counter & 0x0F)),
            (uint8_t)(0x30 | (counter & 0x0F)),
        };
        printf("SEND  %02X %02X %02X\n", buf[0], buf[1], buf[2]);

#ifdef BRINGUP_BURST
        gpio_put(PIN_CS, 0);
        spi_write_blocking(spi1, buf, 3);
        gpio_put(PIN_CS, 1);
#else
        for (int i = 0; i < 3; i++) {
            gpio_put(PIN_CS, 0);
            spi_write_blocking(spi1, &buf[i], 1);
            gpio_put(PIN_CS, 1);
            sleep_us(50);
        }
#endif

        counter++;
        sleep_ms(500);
    }
}
