// slave_engine.h
//
// Sample-clocked audio engine. Intended to run on core1 forever while core0
// runs slave_spi_rx_run(). Each tick it drains any register-write events
// core0 forwarded over the inter-core FIFO, applies them to the chip
// emulator via `ops`, renders one sample, and writes it to the PWM output.
#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

typedef struct {
    // clock_preset: chip-specific index into that slave's clock table
    // (0 = the chip's most common real-world clock). See VGMSPI_OP_RESET.
    void (*reset)(uint8_t clock_preset);
    // port distinguishes which wire opcode triggered this write (0/1 for
    // VGMSPI_OP_WRITE0/WRITE1; 2-5 for the SCC-only opcodes; 6/7 for the
    // Sega PCM-only upload opcodes -- see protocol/vgm_spi_protocol.h).
    // Most chips only ever receive one or two of these values and can
    // ignore the rest.
    void (*write)(uint8_t port, uint8_t reg, uint8_t data);
    // Renders and returns exactly one sample (roughly 12-bit signed
    // headroom -- see audio_pwm.h). Called once per current sample_rate_hz
    // tick (see sample_rate_hz below for how that rate can change).
    int16_t (*render)(void);
    // Returns the correct render/output sample rate (Hz) for a given
    // clock_preset. Called once right after reset(clock_preset) -- at
    // startup and again on every VGMSPI_OP_RESET -- so slave_audio_engine_run
    // can re-pace its tick loop to match. For chips that resample
    // internally at a fixed output rate regardless of the emulated clock
    // (SN76489/AY-3-8910/SCC: see their tick-budget-per-sample math), this
    // just returns the same constant for every preset. For the ymfm-backed
    // chips (YM2612/YM2413/YM2151/YM2203) and Sega PCM, which render at
    // their own clock-derived native rate instead of resampling, this
    // actually varies by preset -- getting it wrong for a given VGM file's
    // real clock shows up as a uniform pitch/tempo shift, not a crash or
    // audible glitch, so it's easy to miss without a reference tone.
    uint32_t (*sample_rate_hz)(uint8_t clock_preset);
} chip_ops_t;

// chip_name is used only for the startup banner and RESET/MUTE log lines
// (see docs/design-notes.md "ログの確認方法"); pass e.g. "SN76489". RESET
// and MUTE are logged unconditionally (rare -- once per song at most, so
// the printf cost doesn't threaten real-time audio). Per-write logging is
// far too frequent for that and is only compiled in when the CMake option
// VGM_SLAVE_VERBOSE_LOG is ON, specifically for bring-up debugging with the
// understanding that it WILL glitch playback -- see slave_common/CMakeLists.txt.
//
// Calls ops->reset(0) and ops->sample_rate_hz(0) itself, in that order, to
// seed the pacing loop before the first VGMSPI_OP_RESET arrives -- note
// this means sample_rate_hz(0) must be safe to call right after reset(0)
// specifically (some chips' sample_rate_hz depends on state reset()
// configures, e.g. YM2203's fidelity mode; don't precompute it in main.c
// before this function has had a chance to call reset() first). After
// startup, the loop re-paces itself to whatever ops->sample_rate_hz(preset)
// returns each time a VGMSPI_OP_RESET arrives.
//
// Never returns.
void slave_audio_engine_run(const chip_ops_t *ops, uint audio_pin, const char *chip_name) __attribute__((noreturn));
