// Minimal SPI bring-up test -- SLAVE side. No chip emulation, no audio, no
// multicore: single core, just waits for 3 bytes under CS and prints them
// via USB CDC / UART, over the *original* slave pin assignment
// (docs/circuit.md). Pairs with ../../master.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define PIN_SCK 10
#define PIN_MOSI 8 // this board's RX -- receives the master's MOSI
#define PIN_CS 9
#define BAUD_HZ (4 * 1000 * 1000)

int main(void) {
    stdio_init_all();
    sleep_ms(3000); // give a USB CDC terminal time to attach before we start printing
#ifdef BRINGUP_MODE1
    const char *mode = "SPI mode 1 (CPHA=1)";
#else
    const char *mode = "SPI mode 0 (CPHA=0)";
#endif
    printf("\n=== SPI bring-up TEST SLAVE (SCK=GPIO%d MOSI/RX=GPIO%d CS=GPIO%d, %d Hz, %s) ===\n",
           PIN_SCK, PIN_MOSI, PIN_CS, BAUD_HZ, mode);

    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);

    spi_init(spi1, BAUD_HZ);
    spi_set_slave(spi1, true);
#ifdef BRINGUP_MODE1
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
#else
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
#endif

    while (spi_is_readable(spi1)) (void)spi_get_hw(spi1)->dr; // flush any startup garbage

    uint32_t n = 0;
    for (;;) {
        uint8_t buf[3];
        for (int i = 0; i < 3; i++) {
            while (!spi_is_readable(spi1)) tight_loop_contents();
            buf[i] = (uint8_t)spi_get_hw(spi1)->dr;
        }
        printf("RECV#%-4lu %02X %02X %02X\n", (unsigned long)n++, buf[0], buf[1], buf[2]);
    }
}
