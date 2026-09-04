// audio_pwm.h
//
// PWM audio output. Carrier runs at sysclk/1024 (~122kHz @ 125MHz sysclk),
// giving 10-bit amplitude resolution; a 2-pole RC low-pass on the board
// (see docs/circuit.md) removes the carrier, leaving audio.
#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

#define AUDIO_PWM_RESOLUTION_BITS 10
#define AUDIO_PWM_WRAP ((1u << AUDIO_PWM_RESOLUTION_BITS) - 1)

void audio_pwm_init(uint pin);

// sample is centered at 0; headroom is intentionally generous (roughly
// 12-bit, i.e. -2048..2047) so chip emulators summing multiple internal
// channels don't need to worry about clipping before this stage clamps and
// rescales down to the 10-bit PWM duty range.
void audio_pwm_write(uint pin, int16_t sample);
