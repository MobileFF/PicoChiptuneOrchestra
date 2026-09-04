// YM2203 (OPN: 3 FM channels + AY-3-8910-compatible SSG) emulator: thin C
// wrapper around the vendored ymfm library. Single register port, like
// YM2151/YM2413 -- VGM command 0x55 aa dd maps straight through.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// clock/24 (ymfm's OPN_FIDELITY_MIN) for the default (3993600 Hz) clock
// preset. Measured via tools/probes/ym2151_ym2203_probe.cpp -- see
// ym2203_sample_rate_hz() for the other preset actually used at runtime.
// This is a much higher native rate than YM2612 (~166kHz vs ~53kHz), but
// per-second CPU cost measured comparable or lower in host-side
// benchmarking -- real-time behavior on RP2040 hardware is unverified,
// likely the riskiest of the FM slaves in this repo. See
// docs/design-notes.md.
#define YM2203_SAMPLE_RATE_HZ 166400u

void ym2203_reset(uint8_t clock_preset);
void ym2203_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t ym2203_render(void);
// 3993600 Hz (preset 0, common on PC-88/PC-98) vs 4000000 Hz (preset 1,
// common on several arcade boards) -- close but not identical, and unlike
// SN76489/AY-3-8910 this chip does NOT resample internally, so getting the
// preset wrong shows up as a uniform pitch/tempo shift across the whole
// song. See chip_ops_t.sample_rate_hz's doc comment.
uint32_t ym2203_sample_rate_hz(uint8_t clock_preset);

#ifdef __cplusplus
}
#endif
