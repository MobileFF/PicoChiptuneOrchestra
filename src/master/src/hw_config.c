// hw_config.c
//
// Board wiring glue required by the vendored FatFs_SPI library
// (third_party/no-OS-FatFS-SD-SPI-RPi-Pico): tells it which SPI peripheral
// and GPIOs the SD card is on. See docs/circuit.md for the physical wiring
// (matches the Pico's conventional SPI0 pins).
#include "hardware/spi.h"
#include "sd_card.h"
#include "spi0_bus_lock.h"

// 2026-10-04: briefly dropped to 4 MHz to test whether "Illegal command
// CMD:17 response 0x4" (an SDXC card's SPI controller rejecting a plain
// read-single-block command outright) was a signal-integrity/clock-margin
// problem -- it was not (identical failure, same sector, at both 4 MHz and
// 20 MHz), so back to 20 MHz. See design-notes.md for the investigation.
#define SD_SPI_BAUD_HZ SPI0_BUS_BAUD_HZ

static spi_t s_spi = {
    .hw_inst = spi0,
    .miso_gpio = 16,
    .mosi_gpio = 19,
    .sck_gpio = 18,
    .baud_rate = SD_SPI_BAUD_HZ,
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
