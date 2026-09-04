// SN76489 slave firmware (RP2040-Zero). core0 receives SPI command frames
// from the master and forwards them to core1; core1 runs the sample-clocked
// chip emulator + PWM audio output. Pin assignments: docs/circuit.md.
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "slave_spi_rx.h"
#include "slave_engine.h"
#include "chip_sn76489.h"

// SPI1 group (valid RX/CSn/SCK for spi1 per RP2040 GPIO function table).
// GPIO 8/9/10 are broken out on the RP2040-Zero pin header (20/21/22 are
// not) and clear its default UART (0/1) and WS2812 (16) pins -- see
// docs/circuit.md.
#define PIN_SPI_MOSI 8  // slave RX <- master MOSI (SPI1 RX)
#define PIN_SPI_CS   9  // SPI1 CSn
#define PIN_SPI_SCK  10 // SPI1 SCK
#define PIN_AUDIO_PWM 11 // PWM out; GPIO11 = SPI1 TX, unused by this slave, on the RP2040-Zero header

static const chip_ops_t sn76489_ops = {
    .reset = sn76489_reset,
    .write = sn76489_write,
    .render = sn76489_render,
    .sample_rate_hz = sn76489_sample_rate_hz,
};

static void core1_entry(void) {
    slave_audio_engine_run(&sn76489_ops, PIN_AUDIO_PWM, "SN76489");
}

int main(void) {
    stdio_init_all();
    multicore_launch_core1(core1_entry);
    slave_spi_rx_run(1, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_CS, VGMSPI_MASK_SN76489);
    return 0;
}
