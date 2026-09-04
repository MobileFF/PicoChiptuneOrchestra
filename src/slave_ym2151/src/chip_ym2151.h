// YM2151 (OPM, arcade/PC-88 era FM) emulator: thin C wrapper around the
// vendored ymfm library. Same approach as chip_ym2612.h/.cpp.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// clock/64. Measured via ymfm's ym2151::sample_rate() in
// tools/probes/ym2151_ym2203_probe.cpp for the default (3579545 Hz) clock
// preset -- see ym2151_sample_rate_hz() for the other presets actually used
// at runtime. Comparable per-second CPU cost to YM2612 in host-side
// benchmarking (see docs/design-notes.md) -- real-time behavior on RP2040
// hardware is otherwise unverified, same caveat as slave_ym2612.
#define YM2151_SAMPLE_RATE_HZ 55930u

void ym2151_reset(uint8_t clock_preset);
void ym2151_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t ym2151_render(void);
// Raw L/R ymfm output for one sample, before ym2151_render()'s >>6 + clamp.
// For offline analysis only (tools/offline_render). Advances the chip one
// sample: call this OR ym2151_render() per output sample, not both.
void ym2151_render_raw(int32_t *l, int32_t *r);
// Real-world YM2151 clock varies by system (3579545 Hz and 4000000 Hz are
// both common -- see CLOCK_PRESETS in chip_ym2151.cpp); unlike
// SN76489/AY-3-8910, this chip does NOT resample internally, so getting
// this wrong for a given VGM file's actual clock shows up as a uniform
// pitch shift across the whole song (~192 cents, nearly 2 semitones, for
// the 3579545-vs-4000000 case) rather than a crash or glitch. See
// chip_ops_t.sample_rate_hz's doc comment.
uint32_t ym2151_sample_rate_hz(uint8_t clock_preset);

#ifdef __cplusplus
}
#endif
