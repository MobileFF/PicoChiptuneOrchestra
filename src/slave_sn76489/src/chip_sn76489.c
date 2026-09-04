#include "chip_sn76489.h"
#include <string.h>

// SN76489 divides its input clock by 16 before the per-channel period
// counters; all internal timing below is expressed in units of that
// clock/16 tick, matching the datasheet formula f_tone = clock / (32 * N).
#define CLOCK_TICK_DIV 16

// 2dB-per-step attenuation table (SN76489 volume registers are 4-bit,
// 0 = loudest, 15 = silent). Values are an approximation scaled so four
// simultaneously-loud channels stay within audio_pwm's ~12-bit headroom.
static const int16_t VOL_TABLE[16] = {
    480, 381, 303, 241, 191, 152, 121, 96,
    76,  60,  48,  38,  30,  24,  19,  0
};

static const uint32_t CLOCK_PRESETS[3] = {
    3579545, // 0: NTSC (Sega Master System / Mega Drive PSG / Game Gear) -- default
    3546893, // 1: PAL
    4000000, // 2: common arcade clock
};

typedef struct {
    uint16_t freq[3];
    int32_t counter[3];
    uint8_t out[3];
    uint8_t atten[3];

    uint8_t noise_ctrl; // bits0-1 rate, bit2 = 1:white / 0:periodic
    uint8_t noise_atten;
    uint16_t lfsr;
    int32_t noise_counter;
    uint8_t noise_out;

    uint8_t latched_reg; // 0..7

    uint32_t sample_rate_hz;
    uint32_t tick_whole;  // whole clock/16 ticks per output sample
    uint32_t tick_rem;    // remainder numerator, denom = CLOCK_TICK_DIV*sample_rate_hz
    uint32_t tick_frac_acc;
} sn76489_state_t;

static sn76489_state_t st;

static void apply_low(uint8_t reg, uint8_t val4) {
    switch (reg) {
        case 0: st.freq[0] = (uint16_t)((st.freq[0] & 0x3F0) | val4); break;
        case 1: st.atten[0] = val4; break;
        case 2: st.freq[1] = (uint16_t)((st.freq[1] & 0x3F0) | val4); break;
        case 3: st.atten[1] = val4; break;
        case 4: st.freq[2] = (uint16_t)((st.freq[2] & 0x3F0) | val4); break;
        case 5: st.atten[2] = val4; break;
        case 6: st.noise_ctrl = val4 & 0x07; st.lfsr = 0x8000; break;
        case 7: st.noise_atten = val4; break;
        default: break;
    }
}

static void apply_high(uint8_t reg, uint8_t val6) {
    // Only the tone-frequency registers use the high 6 bits; a second byte
    // after a volume/noise register (rare in real streams) just re-applies
    // its low 4 bits, mirroring real hardware's 4-bit-wide latch behavior.
    switch (reg) {
        case 0: st.freq[0] = (uint16_t)((val6 << 4) | (st.freq[0] & 0x0F)); break;
        case 2: st.freq[1] = (uint16_t)((val6 << 4) | (st.freq[1] & 0x0F)); break;
        case 4: st.freq[2] = (uint16_t)((val6 << 4) | (st.freq[2] & 0x0F)); break;
        default: apply_low(reg, val6 & 0x0F); break;
    }
}

void sn76489_reset(uint8_t clock_preset) {
    memset(&st, 0, sizeof(st));
    uint32_t clock_hz = CLOCK_PRESETS[clock_preset < 3 ? clock_preset : 0];
    st.sample_rate_hz = SN76489_SAMPLE_RATE_HZ;
    uint32_t denom = CLOCK_TICK_DIV * st.sample_rate_hz;
    st.tick_whole = clock_hz / denom;
    st.tick_rem = clock_hz % denom;
    for (int ch = 0; ch < 3; ch++) st.atten[ch] = 0x0F;
    st.noise_atten = 0x0F;
    st.lfsr = 0x8000;
}

void sn76489_write(uint8_t port, uint8_t reg, uint8_t data) {
    (void)port;
    (void)reg;
    if (data & 0x80) {
        st.latched_reg = (data >> 4) & 0x07;
        apply_low(st.latched_reg, data & 0x0F);
    } else {
        apply_high(st.latched_reg, data & 0x3F);
    }
}

int16_t sn76489_render(void) {
    uint32_t denom = CLOCK_TICK_DIV * st.sample_rate_hz;
    uint32_t budget = st.tick_whole;
    st.tick_frac_acc += st.tick_rem;
    if (st.tick_frac_acc >= denom) {
        st.tick_frac_acc -= denom;
        budget += 1;
    }

    for (int ch = 0; ch < 3; ch++) {
        int32_t n = st.freq[ch] == 0 ? 1 : st.freq[ch];
        st.counter[ch] -= (int32_t)budget;
        while (st.counter[ch] <= 0) {
            st.counter[ch] += n;
            st.out[ch] ^= 1;
        }
    }

    uint32_t nrate = st.noise_ctrl & 0x03;
    int32_t nperiod;
    if (nrate == 3) {
        nperiod = st.freq[2] == 0 ? 1 : st.freq[2];
    } else {
        static const int32_t FIXED_NPERIOD[3] = {32, 64, 128}; // clock/512,1024,2048 in clock/16 units
        nperiod = FIXED_NPERIOD[nrate];
    }
    st.noise_counter -= (int32_t)budget;
    while (st.noise_counter <= 0) {
        st.noise_counter += nperiod;
        uint16_t old = st.lfsr;
        uint16_t fb = (st.noise_ctrl & 0x04) ? (uint16_t)((old & 1) ^ ((old >> 3) & 1)) : (uint16_t)(old & 1);
        st.lfsr = (uint16_t)((old >> 1) | (fb << 15));
    }
    st.noise_out = st.lfsr & 1;

    int32_t mix = 0;
    for (int ch = 0; ch < 3; ch++) {
        int16_t v = VOL_TABLE[st.atten[ch]];
        mix += st.out[ch] ? v : -v;
    }
    int16_t nv = VOL_TABLE[st.noise_atten];
    mix += st.noise_out ? nv : -nv;

    return (int16_t)mix;
}

uint32_t sn76489_sample_rate_hz(uint8_t clock_preset) {
    (void)clock_preset;
    return SN76489_SAMPLE_RATE_HZ;
}
