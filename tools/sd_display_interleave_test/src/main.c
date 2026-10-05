// Standalone SD + display concurrency stress test -- MASTER Pico only.
//
// Isolates ONE specific question, with none of the real firmware's other
// moving parts (no config parsing, no slave bus, no vgm_player, no settle
// window): does core1 CONTINUOUSLY, ASYNCHRONOUSLY redrawing the status
// display -- forever, with no synchronization to core0 beyond whatever the
// real drivers already do (spi0_bus_lock for the TFT build; nothing at all
// needed for the OLED build, a separate I2C0 bus) -- while core0
// continuously hammers the SD card, ever produce a FatFs/SD error?
//
// 2026-10-05 revision: the FIRST version of this test ran everything on
// ONE core, strictly alternating "draw a frame, then do one SD read" --
// and passed cleanly, thousands of iterations, on both OLED and TFT
// builds. But that is NOT what the real firmware actually does: there,
// core1's render loop runs forever on its OWN, truly concurrently with
// whatever core0 happens to be doing at that exact instant -- the two
// cores' SPI0/I2C0 transactions can land at ANY relative offset, including
// overlapping in time, which a single-core "alternate in lockstep" loop
// can never reproduce. This revision launches a real core1 that redraws
// forever with no pacing, while core0 hammers the SD card with no pacing
// either, so the full space of relative timings actually gets explored --
// matching the real concurrency pattern the 2026-10 FR_DISK_ERR
// investigation's main-firmware tests kept reproducing (non-deterministic:
// the exact point an isolated run first failed varied call to call, which
// is itself a strong hint that true dual-core timing, not any single
// deterministic code path, is what matters here -- see
// docs/design-notes.md).
//
// 2026-10-05, second revision: dual-core SD+display concurrency alone
// (both builds, several minutes, zero failures) turned out NOT to
// reproduce it either -- so the one remaining thing the real firmware does
// that no version of this test had covered yet is SPI1 traffic to the
// slave boards (slave_bus_init()/slave_bus_reset(), toggling SCK/MOSI on
// GPIO10/11 plus CS on GPIO12/13/14/15/20/21/22/26 -- physically close to
// SPI0's own GPIO16-19 on this board's wiring). Added here: core0 now also
// pulses every known chip's reset over SPI1 once per iteration, right
// alongside its SD read, testing whether SPI1 activity -- a completely
// separate RP2040 peripheral, but on nearby pins -- is what's actually
// disturbing SPI0 through crosstalk/ground bounce rather than anything
// about the display at all.
//
// Links the REAL drivers (master/src/hw_config.c for SD, master/src/
// st7735.c or ssd1306.c for the display, master/src/slave_bus.c for SPI1,
// master/src/spi0_bus_lock.c for SPI0 arbitration), not reimplementations.
//
// Two builds from this one source, chosen by TEST_USE_TFT (see
// CMakeLists.txt): TEST_USE_TFT=0 is SD+OLED (I2C0 -- physically separate
// bus from the SD card entirely); TEST_USE_TFT=1 is SD+TFT (SPI0 -- shares
// the SD card's own physical bus, CS/DC/RST on GPIO3/4/5). Running the same
// test on both isolates "is this specifically about sharing SPI0" from
// "does ANY concurrent display activity, on ANY bus, ever disturb SD
// reads".
//
// Wiring: identical to the real firmware -- docs/circuit.md section 1.
//   SD:  MISO->GPIO16  CS->GPIO17  SCK->GPIO18  MOSI->GPIO19  (SPI0)
//   TFT: CS->GPIO3  DC->GPIO4  RST->GPIO5  SCK/MOSI shared with SD (SPI0)
//   OLED: SDA->GPIO0  SCL->GPIO1  (I2C0 -- untouched by TFT build)
//
// How to use:
//   1. Flash sd_oled_interleave_test.uf2 or sd_tft_interleave_test.uf2
//      (hold BOOTSEL, copy, reboot).
//   2. Open the USB CDC serial port.
//   3. Watch the running sd_ok/sd_fail/frame counters (core0 prints them
//      periodically). Leave it running for several minutes -- an
//      intermittent, timing-dependent issue may take many thousands of
//      iterations on BOTH cores before the right relative offset happens.
//   4. When done, re-flash this project's own firmware/pico1/master.uf2.
//
// Build: see README.md.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/i2c.h"
#include "ff.h"

#include "spi0_bus_lock.h"
#include "slave_bus.h"
#include "vgm_chips.h"

#ifndef TEST_USE_TFT
#define TEST_USE_TFT 0
#endif

#if TEST_USE_TFT
#include "hardware/spi.h"
#include "st7735.h"
#include "tft_pins.h"
#else
#include "ssd1306.h"
#define OLED_I2C     i2c0
#define OLED_SDA_PIN 0
#define OLED_SCL_PIN 1
#define OLED_I2C_HZ  400000
#define OLED_ADDR    0x3C
#endif

// Shared counters -- core1 only ever increments s_frame, core0 only ever
// reads it for the periodic status line; core0 owns sd_ok/sd_fail
// entirely. No locking: these are progress counters for a human to watch,
// not correctness-critical state, so a torn read racing an increment
// (impossible anyway at this width on this CPU) would not matter either
// way.
static volatile uint32_t s_frame = 0;

// core1: redraws the display forever, with NO pacing and NO synchronization
// to core0 beyond whatever spi0_bus_lock.c already does inside st7735.c
// (TFT build only -- the OLED build's ssd1306.c never touches SPI0 at all,
// so core1 here is running on a bus core0 never uses, same as the real
// firmware's OLED backend).
static void core1_main(void) {
    char line[32];
    for (;;) {
#if TEST_USE_TFT
        st7735_clear();
        snprintf(line, sizeof(line), "frame %lu", (unsigned long)s_frame);
        st7735_text(0, 0, line);
        st7735_show();
#else
        ssd1306_clear();
        snprintf(line, sizeof(line), "frame %lu", (unsigned long)s_frame);
        ssd1306_text(0, 0, line);
        ssd1306_show();
#endif
        s_frame++;
    }
}

int main(void) {
    stdio_init_all();
    for (int s = 3; s > 0; s--) { printf("  starting in %d s ...\n", s); sleep_ms(1000); }

    printf("\n=== SD + %s CONCURRENCY stress test (real dual-core) ===\n",
           TEST_USE_TFT ? "TFT (shares SPI0 with SD)" : "OLED (separate I2C0 bus)");
    printf("core1 redraws forever, unpaced; core0 hammers the SD card, unpaced.\n\n");

    // Must be ready before ANY SPI0 access, and before core1 launches --
    // see spi0_bus_lock.h. Harmless (never actually contended) in the OLED
    // build, since OLED never touches SPI0 at all.
    spi0_bus_lock_init();

    static FATFS fs;
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("f_mount FAILED: FatFs error %d -- check wiring, halting.\n", fr);
        for (;;) tight_loop_contents();
    }
    printf("SD card mounted.\n");

#if TEST_USE_TFT
    // SD mount above already brought SPI0 up at this project's shared
    // baud/format -- see st7735_init()'s own doc comment on why passing
    // true here (not re-initializing the peripheral) matters.
    st7735_init(spi0, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO, true);
    printf("TFT init done (always \"succeeds\" -- write-only panel, see st7735_init()'s doc comment).\n");
#else
    i2c_init(OLED_I2C, OLED_I2C_HZ);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);
    bool oled_ok = ssd1306_init(OLED_I2C, OLED_ADDR);
    printf("OLED init: %s\n", oled_ok ? "panel ACKed" : "no ACK (not wired? continuing anyway)");
#endif

    // Default routing table (slave_bus.c's own static s_routes[], never
    // touched by player_config.c here -- this test skips config parsing
    // entirely) already has every real chip "present" with its real CS
    // GPIO, so slave_bus_has_chip() below exercises the exact same pins
    // the real firmware's slave settle window does.
    slave_bus_init();
    printf("slave bus (SPI1) initialized.\n");

    printf("Launching core1 (display redraw loop)...\n\n");
    multicore_launch_core1(core1_main);

    uint32_t iter = 0, sd_ok = 0, sd_fail = 0;
    static DIR dir;
    static FILINFO info;

    for (;;) {
        iter++;

        // SPI1 traffic (slave chip resets) immediately before the SD read,
        // same relative ordering as main.c's own slave settle window --
        // see this file's top comment on why this was added.
        for (int c = 0; c < VGM_CHIP_COUNT; c++)
            if (slave_bus_has_chip((vgm_chip_id_t)c))
                slave_bus_reset((vgm_chip_id_t)c, 0);

        FRESULT r = f_findfirst(&dir, &info, "0:", "*");
        if (r == FR_OK) {
            f_closedir(&dir);
            sd_ok++;
        } else {
            sd_fail++;
            printf("iter %lu (frame %lu): f_findfirst(\"0:\") FAILED: FatFs error %d  (running: sd_ok=%lu sd_fail=%lu)\n",
                   (unsigned long)iter, (unsigned long)s_frame, r, (unsigned long)sd_ok, (unsigned long)sd_fail);
        }

        if (iter % 500 == 0) {
            printf("iter=%lu  frame=%lu  sd_ok=%lu  sd_fail=%lu\n",
                   (unsigned long)iter, (unsigned long)s_frame, (unsigned long)sd_ok, (unsigned long)sd_fail);
        }
    }
}
