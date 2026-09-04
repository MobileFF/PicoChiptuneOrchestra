// YM2151 slave firmware (RP2040-Zero, or Pico 2/RP2350 -- see below). Same
// core0/core1 split as slave_sn76489; see that firmware's main.c for the
// rationale.
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "slave_spi_rx.h"
#include "slave_engine.h"
#include "slave_overclock.h"
#include "chip_ym2151.h"

#ifdef VGM_YM2151_USB_DIAG
#include <stdio.h>
#include "hardware/clocks.h"
#endif

#define PIN_SPI_MOSI 8  // slave RX <- master MOSI (SPI1 RX)
#define PIN_SPI_CS   9  // SPI1 CSn
#define PIN_SPI_SCK  10 // SPI1 SCK
#define PIN_AUDIO_PWM 11 // PWM out; GPIO11 = SPI1 TX, unused by this slave, on the RP2040-Zero header

static const chip_ops_t ym2151_ops = {
    .reset = ym2151_reset,
    .write = ym2151_write,
    .render = ym2151_render,
    .sample_rate_hz = ym2151_sample_rate_hz,
};

static void core1_entry(void) {
    slave_audio_engine_run(&ym2151_ops, PIN_AUDIO_PWM, "YM2151");
}

// Optional boot hold-off before core1 / SPI RX start. Diagnostic aid for the
// "song plays flat" (missed one-shot RESET) hunt: pair with the master's
// VGM_MASTER_BOOT_DELAY_S so the slave is provably listening on SPI before the
// master sends its RESET burst. 0 = no delay (normal). -DVGM_YM2151_BOOT_DELAY_MS=N.
#ifndef VGM_YM2151_BOOT_DELAY_MS
#define VGM_YM2151_BOOT_DELAY_MS 0
#endif

int main(void) {
    slave_rp2350_overclock();
    stdio_init_all();
#if VGM_YM2151_BOOT_DELAY_MS > 0
    sleep_ms(VGM_YM2151_BOOT_DELAY_MS);
#endif

#ifdef VGM_YM2151_USB_DIAG
    // Diagnostic build. IMPORTANT: do NOT block here waiting for a USB
    // terminal -- that delays slave_spi_rx_run() below and the slave would
    // miss the master's one-shot RESET frame (sent in a <1ms burst at song
    // start), which is exactly the bug under investigation. Just print a
    // banner (it may be missed if no terminal is attached yet -- that's
    // fine, the once/sec RATE CHECK line from slave_engine shows the state,
    // and it now also reports how many RESET frames have been received).
    //   RATE CHECK "@ 62500 Hz" + "reset_rx>=1"  -> RESET landed; flat pitch
    //     is elsewhere (check elapsed vs target on the same line = keeping up?)
    //   RATE CHECK "@ 55930 Hz" + "reset_rx=0"   -> RESET never arrived.
    printf("\n=== YM2151 slave USB DIAG ===  sys clock = %lu Hz (target %d kHz)\n",
           (unsigned long)clock_get_hz(clk_sys), VGM_RP2350_SYSCLK_KHZ);
#endif

    multicore_launch_core1(core1_entry);
    slave_spi_rx_run(1, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_CS, VGMSPI_MASK_YM2151);
    return 0;
}
