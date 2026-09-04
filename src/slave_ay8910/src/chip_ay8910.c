#include "chip_ay8910.h"
#include <string.h>
#include <stdbool.h>

// One-pole smoothing of each channel's audio-domain output level before
// mixing, instead of the instant step every envelope tick / level register
// write otherwise applies. 0 (default) = OFF, byte-identical to the
// original instant-step behaviour -- this is a prototyping hook for the
// "Hot Summer Riding" investigation (see tools/offline_render/ and
// docs/design-notes.md): the raw instant-step output attacks harder than a
// reference recording of the real track (measured as extra onset density in
// a busy passage). NOT enabled in shipped firmware; set via
// -DAY8910_LEVEL_SLEW_HZ=<hz> to sweep it offline before considering
// whether/how to turn it on for real.
#ifndef AY8910_LEVEL_SLEW_HZ
#define AY8910_LEVEL_SLEW_HZ 0
#endif
#if AY8910_LEVEL_SLEW_HZ > 0
#include <math.h>
#endif

// AY-3-8910 tone counters run at clock/8; a channel toggles its output once
// every `period` such ticks, giving f_tone = clock/(16*period) (two toggles
// per cycle) -- matches the datasheet formula. The noise generator and
// envelope generator share the same clock/8 tick base for simplicity (their
// per-datasheet divisors of 16 and 256 are applied as extra multipliers
// below rather than a second tick base).
#define CLOCK_TICK_DIV 8

// ~2dB/step approximation, scaled so three simultaneously-loud channels
// stay within audio_pwm's headroom (see chip_sn76489.c for the same idea).
static const int16_t VOL_TABLE[16] = {
    0, 20, 28, 40, 56, 80, 113, 160,
    226, 320, 452, 500, 550, 600, 650, 700
};

static const uint32_t CLOCK_PRESETS[3] = {
    1789773, // 0: common (MSX/NES-adjacent) -- default
    1773400, // 1: ZX Spectrum
    2000000, // 2: Amstrad CPC / many arcade boards
};

typedef struct {
    uint8_t fine[3], coarse[3];
    uint16_t period[3];
    int32_t counter[3];
    uint8_t out[3];

    uint8_t noise_period; // 5 bits
    int32_t noise_counter;
    uint32_t lfsr;
    uint8_t noise_out;

    uint8_t mixer; // R7

    uint8_t level[3]; // R8-10: bits0-3 = level, bit4 = use envelope

    uint8_t env_fine, env_coarse;
    uint32_t env_period;
    int32_t env_counter;
    uint8_t env_shape; // R13 bits0-3
    uint32_t env_pos;  // 0..31
    bool env_holding;

    uint32_t sample_rate_hz;
    uint32_t tick_whole; // whole clock/8 ticks per output sample
    uint32_t tick_rem;   // remainder numerator, denom = CLOCK_TICK_DIV*sample_rate_hz
    uint32_t tick_frac_acc;

#if AY8910_LEVEL_SLEW_HZ > 0
    float level_smooth[3]; // per-channel smoothed VOL_TABLE value
    float level_alpha;
#endif
} ay8910_state_t;

static ay8910_state_t st;

static void recompute_period(int ch) {
    st.period[ch] = (uint16_t)(((st.coarse[ch] & 0x0F) << 8) | st.fine[ch]);
}

static void env_restart(void) {
    st.env_pos = 0;
    st.env_holding = false;
    // Also reload the period counter: real hardware restarts the envelope
    // counter on an R13 write, so the first step lands exactly one period
    // after the retrigger. Without this, env_counter kept whatever value it
    // was frozen at (env_holding paused its decrement), so the first step
    // came a RANDOM 0..(32*EP/budget) samples later -- up to ~110 ms for the
    // long EP this song uses on the melody voice, heard as the melody
    // articulating late/sluggishly against the (envelope-free) drums.
    st.env_counter = (st.env_period == 0 ? 1 : (int32_t)st.env_period) * 32;
}

void ay8910_reset(uint8_t clock_preset) {
    memset(&st, 0, sizeof(st));
    st.sample_rate_hz = AY8910_SAMPLE_RATE_HZ;
    uint32_t clock_hz = CLOCK_PRESETS[clock_preset < 3 ? clock_preset : 0];
    uint32_t denom = CLOCK_TICK_DIV * st.sample_rate_hz;
    st.tick_whole = clock_hz / denom;
    st.tick_rem = clock_hz % denom;
    st.mixer = 0xFF; // all tone+noise disabled at power-on
    for (int ch = 0; ch < 3; ch++) st.period[ch] = 1;
    st.lfsr = 1;
#if AY8910_LEVEL_SLEW_HZ > 0
    st.level_alpha = 1.0f - expf(-2.0f * 3.14159265f *
                                  (float)AY8910_LEVEL_SLEW_HZ / (float)st.sample_rate_hz);
#endif
}

void ay8910_write(uint8_t port, uint8_t reg, uint8_t data) {
    (void)port;
    switch (reg & 0x0F) {
        case 0: st.fine[0] = data; recompute_period(0); break;
        case 1: st.coarse[0] = data & 0x0F; recompute_period(0); break;
        case 2: st.fine[1] = data; recompute_period(1); break;
        case 3: st.coarse[1] = data & 0x0F; recompute_period(1); break;
        case 4: st.fine[2] = data; recompute_period(2); break;
        case 5: st.coarse[2] = data & 0x0F; recompute_period(2); break;
        case 6: st.noise_period = data & 0x1F; break;
        case 7: st.mixer = data; break;
        case 8: st.level[0] = data & 0x1F; break;
        case 9: st.level[1] = data & 0x1F; break;
        case 10: st.level[2] = data & 0x1F; break;
        case 11: st.env_fine = data; st.env_period = ((uint32_t)st.env_coarse << 8) | st.env_fine; break;
        case 12: st.env_coarse = data; st.env_period = ((uint32_t)st.env_coarse << 8) | st.env_fine; break;
        case 13: st.env_shape = data & 0x0F; env_restart(); break;
        default: break; // R14/R15 I/O ports: no device attached
    }
}

static void tone_tick(uint32_t budget) {
    for (int ch = 0; ch < 3; ch++) {
        int32_t n = st.period[ch] == 0 ? 1 : st.period[ch];
        st.counter[ch] -= (int32_t)budget;
        while (st.counter[ch] <= 0) {
            st.counter[ch] += n;
            st.out[ch] ^= 1;
        }
    }
}

static void noise_tick(uint32_t budget) {
    // Datasheet divisor is 16*period vs. tone's 16*period-with-two-toggles;
    // multiplying by 2 here keeps both using the same clock/8 tick budget.
    int32_t n = st.noise_period == 0 ? 1 : st.noise_period;
    n *= 2;
    st.noise_counter -= (int32_t)budget;
    while (st.noise_counter <= 0) {
        st.noise_counter += n;
        uint32_t bit = ((st.lfsr ^ (st.lfsr >> 3)) & 1);
        st.lfsr = (st.lfsr >> 1) | (bit << 16);
        st.noise_out = st.lfsr & 1;
    }
}

static uint8_t env_level(void) {
    bool attack = (st.env_shape & 0x04) != 0;
    bool alt = (st.env_shape & 0x02) != 0;
    uint32_t local = st.env_pos & 15;
    bool half2 = st.env_pos >= 16;
    bool rising = attack;
    if (half2 && alt) rising = !rising;
    return rising ? (uint8_t)local : (uint8_t)(15 - local);
}

static void envelope_tick(uint32_t budget) {
    bool cont = (st.env_shape & 0x08) != 0;
    bool hold = (st.env_shape & 0x01) != 0;
    // One envelope step every 256*EP clock cycles (datasheet: the envelope
    // counter is clocked at clock/256, and steps once per EP of those). The
    // shared tick base here is clock/8, so that's 32*EP of these ticks.
    // (This was *16 = clock/128 before, running every envelope exactly one
    // octave too fast -- decay shapes died in half their time, which on a
    // song that leans on the PSG hardware envelope for its bass/pluck notes
    // sounds like that voice dropping out / falling behind the other chip.)
    int32_t n = st.env_period == 0 ? 1 : (int32_t)st.env_period;
    n *= 32;
    if (st.env_holding) return;
    st.env_counter -= (int32_t)budget;
    while (!st.env_holding && st.env_counter <= 0) {
        st.env_counter += n;
        st.env_pos++;
        uint32_t period_len = cont ? 32u : 16u;
        if (st.env_pos >= period_len) {
            if (!cont || hold) {
                st.env_holding = true;
                st.env_pos = period_len - 1;
            } else {
                st.env_pos = 0;
            }
        }
    }
}

int16_t ay8910_render(void) {
    uint32_t denom = CLOCK_TICK_DIV * st.sample_rate_hz;
    uint32_t budget = st.tick_whole;
    st.tick_frac_acc += st.tick_rem;
    if (st.tick_frac_acc >= denom) {
        st.tick_frac_acc -= denom;
        budget += 1;
    }

    tone_tick(budget);
    noise_tick(budget);
    envelope_tick(budget);

    int32_t mix = 0;
    for (int ch = 0; ch < 3; ch++) {
        bool tone_on = !(st.mixer & (1u << ch));
        bool noise_on = !(st.mixer & (1u << (3 + ch)));
        uint8_t tv = tone_on ? st.out[ch] : 1;
        uint8_t nv = noise_on ? st.noise_out : 1;
        bool active = (tv & nv) != 0;
#if AY8910_LEVEL_SLEW_HZ > 0
        uint8_t lvl = (st.level[ch] & 0x10) ? env_level() : (st.level[ch] & 0x0F);
        float target = active ? (float)VOL_TABLE[lvl] : 0.0f;
        st.level_smooth[ch] += st.level_alpha * (target - st.level_smooth[ch]);
        mix += (int32_t)(st.level_smooth[ch] + (st.level_smooth[ch] < 0 ? -0.5f : 0.5f));
#else
        if (!active) continue;
        uint8_t lvl = (st.level[ch] & 0x10) ? env_level() : (st.level[ch] & 0x0F);
        mix += VOL_TABLE[lvl];
#endif
    }
    return (int16_t)mix;
}

uint32_t ay8910_sample_rate_hz(uint8_t clock_preset) {
    (void)clock_preset;
    return AY8910_SAMPLE_RATE_HZ;
}
