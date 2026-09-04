// YM2203 slave firmware (RP2040-Zero, or Pico 2/RP2350 -- see below). Same
// core0/core1 split as slave_sn76489; see that firmware's main.c for the
// rationale. This is the heaviest slave in the repo (see chip_ym2203.h) --
// give it a whole board.
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "slave_spi_rx.h"
#include "slave_engine.h"
#include "slave_overclock.h"
#include "chip_ym2203.h"

#define PIN_SPI_MOSI 8  // slave RX <- master MOSI (SPI1 RX)
#define PIN_SPI_CS   9  // SPI1 CSn
#define PIN_SPI_SCK  10 // SPI1 SCK
#define PIN_AUDIO_PWM 11 // PWM out; GPIO11 = SPI1 TX, unused by this slave, on the RP2040-Zero header

static const chip_ops_t ym2203_ops = {
    .reset = ym2203_reset,
    .write = ym2203_write,
    .render = ym2203_render,
    .sample_rate_hz = ym2203_sample_rate_hz,
};

static void core1_entry(void) {
    slave_audio_engine_run(&ym2203_ops, PIN_AUDIO_PWM, "YM2203");
}

int main(void) {
    // RP2350 overclock (+ optional voltage bump). YM2203's ~166 kHz render
    // rate is the highest in the repo; at 250 MHz it renders slightly flat.
    // ~300 MHz gets very close; >300 MHz needs a core-voltage bump
    // (-DVGM_RP2350_VREG_MV=) to be stable. See docs/design-notes.md §5 and
    // firmware/pico2/README.md. No-op on RP2040.
    slave_rp2350_overclock();
    stdio_init_all();
    multicore_launch_core1(core1_entry);
    slave_spi_rx_run(1, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_CS, VGMSPI_MASK_YM2203);
    return 0;
}
