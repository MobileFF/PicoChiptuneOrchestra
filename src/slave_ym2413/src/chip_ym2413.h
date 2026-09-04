// YM2413 (OPLL, "FM音源簡易版") emulator: thin C wrapper around the vendored
// ymfm library. See chip_ym2612.h/.cpp for the general approach; this chip
// is lighter (2 operators/channel, built-in preset instruments only).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// clock/? for the default (3579545 Hz) clock preset. Measured via ymfm's
// ym2413::sample_rate() in tools/probes/ym2413_probe.cpp -- see
// ym2413_sample_rate_hz() for the other preset actually used at runtime.
#define YM2413_SAMPLE_RATE_HZ 49715u

void ym2413_reset(uint8_t clock_preset);
void ym2413_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t ym2413_render(void);
// Real-world YM2413 clock is overwhelmingly 3579545 Hz (MSX-MUSIC, Sega
// Master System FM unit, etc.); the second preset covers the PAL-console
// variant seen for some SN76489-adjacent hardware (~1% off, same class of
// approximation as SN76489/AY-3-8910's own presets -- see CLOCK_PRESETS in
// chip_ym2413.cpp). Unlike SN76489/AY-3-8910, this chip does NOT resample
// internally, so a wrong preset shows up as a uniform pitch shift across
// the whole song. See chip_ops_t.sample_rate_hz's doc comment.
uint32_t ym2413_sample_rate_hz(uint8_t clock_preset);

#ifdef __cplusplus
}
#endif
