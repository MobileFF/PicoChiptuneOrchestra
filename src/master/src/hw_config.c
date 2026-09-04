// hw_config.c
//
// Board wiring glue required by the vendored FatFs_SPI library
// (third_party/no-OS-FatFS-SD-SPI-RPi-Pico): tells it which SPI peripheral
// and GPIOs the SD card is on. See docs/circuit.md for the physical wiring
// (matches the Pico's conventional SPI0 pins).
#include "hardware/spi.h"
#include "sd_card.h"

static spi_t s_spi = {
    .hw_inst = spi0,
    .miso_gpio = 16,
    .mosi_gpio = 19,
    .sck_gpio = 18,
    .baud_rate = 20 * 1000 * 1000,
    .DMA_IRQ_num = DMA_IRQ_0,
};

static sd_card_t s_sd_card = {
    .pcName = "0:",
    .spi = &s_spi,
    .ss_gpio = 17,
    .use_card_detect = false,
};

size_t sd_get_num(void) { return 1; }
sd_card_t *sd_get_by_num(size_t num) { return (num == 0) ? &s_sd_card : NULL; }

size_t spi_get_num(void) { return 1; }
spi_t *spi_get_by_num(size_t num) { return (num == 0) ? &s_spi : NULL; }
