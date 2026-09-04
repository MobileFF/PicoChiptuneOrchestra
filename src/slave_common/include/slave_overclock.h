// Shared RP2350 (Pico 2) overclock helper for the compute-heavy ymfm slaves
// (YM2151/YM2203/YM2612/SegaPCM). No-op on RP2040.
//
// Tunable from CMake:
//   -DVGM_RP2350_SYSCLK_KHZ=<khz>   system clock (default 250000)
//   -DVGM_RP2350_VREG_MV=<mv>       core voltage in mV, 0 = leave at the SDK
//                                   default 1.10 V (default 0)
//
// RP2350 will not run reliably much past ~300 MHz at 1.10 V. Raising the core
// voltage to ~1.15-1.25 V before the clock bump typically reaches 350-400 MHz.
// Higher voltage means more heat; use the lowest value that is stable. The SDK
// caps the "safe" range at VREG_VOLTAGE_1_30 and this helper clamps to it.
//
// After set_sys_clock_khz(), clk_peri follows clk_sys -- so overclocking also
// clocks the PL022 SPI (and the UART) at the full 300+ MHz, well past what
// they're rated for. Symptom seen: core1 keeps rendering (RATE CHECK still
// prints) but the SPI slave stops decoding frames, so no RESET / register
// writes arrive and the chip plays silence. This helper re-pins clk_peri to
// 48 MHz (PLL_USB) so the peripherals run in spec regardless of the sysclk.
#pragma once

#ifndef VGM_RP2350_SYSCLK_KHZ
#define VGM_RP2350_SYSCLK_KHZ 250000
#endif
#ifndef VGM_RP2350_VREG_MV
#define VGM_RP2350_VREG_MV 0
#endif
// RP2040 sysclk in kHz. 0 = leave at the SDK default (125-133 MHz). Set e.g.
// 250000 to give a light-but-bursty slave (AY-3-8910 tick loops, SCC) enough
// headroom that slave_engine's per-batch FIFO drain never blows the 22.7us
// sample budget -- the "fell_behind" resync that otherwise fires silently
// costs a few ms/loop and, being per-chip and content-dependent, can leave
// two slaves a bounded amount out of step with each other. clk_peri is
// re-pinned to 48 MHz below so the 4 MHz SPI slave keeps decoding.
#ifndef VGM_RP2040_SYSCLK_KHZ
#define VGM_RP2040_SYSCLK_KHZ 0
#endif

#if PICO_RP2350

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

static inline void slave_rp2350_overclock(void) {
#if VGM_RP2350_VREG_MV > 0
    enum vreg_voltage v =
        (VGM_RP2350_VREG_MV >= 1300) ? VREG_VOLTAGE_1_30 :
        (VGM_RP2350_VREG_MV >= 1250) ? VREG_VOLTAGE_1_25 :
        (VGM_RP2350_VREG_MV >= 1200) ? VREG_VOLTAGE_1_20 :
        (VGM_RP2350_VREG_MV >= 1150) ? VREG_VOLTAGE_1_15 :
                                       VREG_VOLTAGE_1_10;
    vreg_set_voltage(v);
    sleep_ms(3); // let the rail settle before raising the clock
#endif
    set_sys_clock_khz(VGM_RP2350_SYSCLK_KHZ, true);

    // Keep clk_peri (PL022 SPI + UART) off the overclocked clk_sys -- run it
    // from PLL_USB at a fixed 48 MHz so the SPI slave keeps decoding frames.
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * 1000 * 1000, 48 * 1000 * 1000);
}

// Unified entry: RP2350 uses the tuned overclock above; RP2040 does nothing
// unless VGM_RP2040_SYSCLK_KHZ was set.
static inline void slave_overclock(void) { slave_rp2350_overclock(); }

#else  // RP2040

#include "pico/stdlib.h"
#include "hardware/clocks.h"

static inline void slave_rp2350_overclock(void) {}

static inline void slave_overclock(void) {
#if VGM_RP2040_SYSCLK_KHZ > 0
    set_sys_clock_khz(VGM_RP2040_SYSCLK_KHZ, true);
    // clk_peri follows clk_sys by default -> overclocking would also clock
    // the 4 MHz SPI slave off-spec and it stops decoding frames (silence).
    // Pin it to PLL_USB / 48 MHz.
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * 1000 * 1000, 48 * 1000 * 1000);
#endif
}

#endif
