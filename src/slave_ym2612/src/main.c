// YM2612 slave firmware (RP2040-Zero, or Pico 2/RP2350 -- see below). Same
// core0/core1 split as slave_sn76489; see that firmware's main.c for the
// rationale. This chip is the heaviest of the four (see
// docs/design-notes.md), so it gets a whole board to itself.
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "slave_spi_rx.h"
#include "slave_engine.h"
#include "slave_overclock.h"
#include "chip_ym2612.h"

#define PIN_SPI_MOSI 8  // slave RX <- master MOSI (SPI1 RX)
#define PIN_SPI_CS   9  // SPI1 CSn
#define PIN_SPI_SCK  10 // SPI1 SCK
#define PIN_AUDIO_PWM 11 // PWM out; GPIO11 = SPI1 TX, unused by this slave, on the RP2040-Zero header

static const chip_ops_t ym2612_ops = {
    .reset = ym2612_reset,
    .write = ym2612_write,
    .render = ym2612_render,
    .sample_rate_hz = ym2612_sample_rate_hz,
};

static void core1_entry(void) {
    slave_audio_engine_run(&ym2612_ops, PIN_AUDIO_PWM, "YM2612");
}

int main(void) {
    slave_rp2350_overclock();
    stdio_init_all();
    multicore_launch_core1(core1_entry);
    slave_spi_rx_run(1, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_CS, VGMSPI_MASK_YM2612);
    return 0;
}
