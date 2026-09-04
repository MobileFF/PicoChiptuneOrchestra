// AY-3-8910 (PSG) software emulator: 3 square-wave tone channels sharing one
// LFSR noise generator, per-channel mixer gating, and one envelope
// generator that can drive any channel's volume.
#pragma once

#include <stdint.h>

#define AY8910_SAMPLE_RATE_HZ 44100

void ay8910_reset(uint8_t clock_preset);
void ay8910_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t ay8910_render(void);
// Always AY8910_SAMPLE_RATE_HZ: see sn76489_sample_rate_hz's doc comment,
// same reasoning (this chip resamples internally too).
uint32_t ay8910_sample_rate_hz(uint8_t clock_preset);
