// SCC (K051649) slave firmware (RP2040-Zero). Same core0/core1 split as
// slave_sn76489; see that firmware's main.c for the rationale.
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "slave_spi_rx.h"
#include "slave_engine.h"
#include "slave_overclock.h"
#include "chip_scc.h"

#define PIN_SPI_MOSI 8  // slave RX <- master MOSI (SPI1 RX)
#define PIN_SPI_CS   9  // SPI1 CSn
#define PIN_SPI_SCK  10 // SPI1 SCK
#define PIN_AUDIO_PWM 11 // PWM out; GPIO11 = SPI1 TX, unused by this slave, on the RP2040-Zero header

static const chip_ops_t scc_ops = {
    .reset = scc_reset,
    .write = scc_write,
    .render = scc_render,
    .sample_rate_hz = scc_sample_rate_hz,
};

static void core1_entry(void) {
    slave_audio_engine_run(&scc_ops, PIN_AUDIO_PWM, "SCC");
}

int main(void) {
    slave_overclock(); // no-op unless VGM_RP2040_SYSCLK_KHZ is set
    stdio_init_all();
    multicore_launch_core1(core1_entry);
    slave_spi_rx_run(1, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_CS, VGMSPI_MASK_SCC);
    return 0;
}
