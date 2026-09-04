// SN76489 (Texas Instruments PSG) software emulator: 3 square-wave tone
// channels + 1 LFSR noise channel, 4-bit (16-step) attenuation each.
// Matches the SMS/Genesis-style byte-serial register protocol.
#pragma once

#include <stdint.h>

#define SN76489_SAMPLE_RATE_HZ 44100

void sn76489_reset(uint8_t clock_preset);
void sn76489_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t sn76489_render(void);
// Always SN76489_SAMPLE_RATE_HZ: this chip resamples internally (its own
// tick-budget-per-sample math in chip_sn76489.c) so the render/output rate
// never needs to change with clock_preset. See chip_ops_t.sample_rate_hz.
uint32_t sn76489_sample_rate_hz(uint8_t clock_preset);
