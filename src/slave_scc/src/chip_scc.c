#include "chip_scc.h"
#include <string.h>
#include <stdbool.h>

// Same idea as chip_ay8910.c's AY8910_LEVEL_SLEW_HZ: one-pole smoothing of
// each channel's volume before it multiplies the waveform sample, instead
// of the instant step every VGMSPI_OP_SCC_VOLUME write (this song alone
// does ~5100 of those -- a software envelope, ~19Hz of stepped volume)
// otherwise applies. 0 (default) = OFF, byte-identical to the original.
// Prototyping hook -- see tools/offline_render/ and docs/design-notes.md.
#ifndef SCC_VOLUME_SLEW_HZ
#define SCC_VOLUME_SLEW_HZ 0
#endif
#if SCC_VOLUME_SLEW_HZ > 0
#include <math.h>
#endif

// The K051649 in an MSX is fed the 3.579545 MHz Z80 bus clock, and the
// waveform frequency is clock / (32 * (period+1)) -- so with real-SCC
// period values, preset 0 is correct. But some VGM rips put HALF that
// (1789772) in the header's K051649 clock field (offset 0x9C), expecting a
// player that folds a /2 into the formula; honouring 3579545 for such a
// file then plays the SCC an octave high. The master reads 0x9C and picks
// the nearest of these, so both conventions come out right. Index-aligned
// with SCC_CLOCK_PRESETS in master/src/vgm_player.c.
static const uint32_t CLOCK_PRESETS[2] = { 3579545u, 1789772u };

typedef struct {
    int8_t wave[5][32];
    uint16_t freq[5];   // 12-bit
    uint8_t volume[5];  // 0-15
    uint8_t key[5];     // key on/off
    uint32_t phase[5];  // 16.16 fixed-point read position into wave[]
    uint32_t step[5];   // phase advance per output sample, cached per channel

    uint32_t clock_hz;

#if SCC_VOLUME_SLEW_HZ > 0
    float vol_smooth[5]; // per-channel smoothed volume (0..15 domain)
    float vol_alpha;
#endif
} scc_state_t;

static scc_state_t st;

// Real hardware: a channel's 32-sample waveform repeats at
// f = clock / (32 * (freq+1)) if freq > 8 (freq <= 8 halts the channel).
// This matches the canonical K051649/SCC formula (Sean Young's SCC doc,
// openMSX, MAME k051649). The 12-bit period divides the 3.58 MHz clock and
// the resulting tick steps the 32-entry waveform pointer once, so one full
// waveform cycle takes 32 * (freq+1) clocks. We render at a fixed
// SCC_SAMPLE_RATE_HZ instead of the native per-chip tick rate, so the
// per-output-sample step scales by clock / (32 * (freq+1) * SCC_SAMPLE_RATE_HZ),
// expressed here in 16.16 phase units over the 32-entry table (<<16 * 32 = 2^21).
//
// step depends only on `freq` (12-bit) and the clock, so precompute all 4096
// values. The former per-freq-write formula did a 64-bit multiply + 64-bit
// divide, which on the Cortex-M0+ (no hardware divide) costs several us
// each -- a FREQ-write-dense song then stacked enough of them into one
// FIFO-drain to blow the engine's 22.7us per-sample budget. A table lookup
// is ~free.
//
// The table is built ONCE for the 3.58 MHz base clock, at engine startup
// (the first scc_reset(0), before any song) -- building it costs ~20ms of
// 64-bit divides and MUST NOT happen once the song is streaming, or core1
// stalls in here long enough for core0's SPI RX to overflow and drop the
// SCC's initial waveform uploads (heard as: SCC goes silent). Preset 1
// (the 1789772 "half clock" convention, ~= base/2) is just `step >> 1`.
static uint32_t s_step_lut[4096];
static bool s_lut_built;
static uint8_t s_clock_shift; // 0 = 3579545, 1 = 1789772

static void build_step_lut(void) {
    uint64_t num = (uint64_t)2097152 * CLOCK_PRESETS[0];
    for (uint32_t f = 0; f < 4096; f++)
        s_step_lut[f] = (uint32_t)(num / ((uint64_t)(f + 1) * 32 * SCC_SAMPLE_RATE_HZ));
    s_lut_built = true;
}

static void recompute_step(int ch) {
    st.step[ch] = s_step_lut[st.freq[ch] & 0x0FFF] >> s_clock_shift;
}

void scc_reset(uint8_t clock_preset) {
    memset(&st, 0, sizeof(st));
    s_clock_shift = (clock_preset == 1) ? 1 : 0;
    st.clock_hz = CLOCK_PRESETS[s_clock_shift];
    if (!s_lut_built) build_step_lut();
    for (int ch = 0; ch < 5; ch++) recompute_step(ch);
#if SCC_VOLUME_SLEW_HZ > 0
    st.vol_alpha = 1.0f - expf(-2.0f * 3.14159265f *
                                (float)SCC_VOLUME_SLEW_HZ / (float)SCC_SAMPLE_RATE_HZ);
#endif
}

static void waveform_write(uint8_t reg, uint8_t data) {
    reg &= 0x7F;
    uint8_t idx = reg & 0x1F;
    if (reg >= 0x60) {
        // Channels 3 and 4 (0-indexed) share one waveform table.
        st.wave[3][idx] = (int8_t)data;
        st.wave[4][idx] = (int8_t)data;
    } else {
        st.wave[reg >> 5][idx] = (int8_t)data;
    }
}

static void freq_write(uint8_t reg, uint8_t data) {
    uint8_t ch = (reg >> 1) & 0x07;
    if (ch >= 5) return;
    if (reg & 1)
        st.freq[ch] = (uint16_t)((st.freq[ch] & 0x0FF) | ((uint16_t)(data & 0x0F) << 8));
    else
        st.freq[ch] = (uint16_t)((st.freq[ch] & 0xF00) | data);
    recompute_step(ch);
}

static void volume_write(uint8_t reg, uint8_t data) {
    uint8_t ch = reg & 0x07;
    if (ch >= 5) return;
    st.volume[ch] = data & 0x0F;
}

static void keyon_write(uint8_t data) {
    for (int ch = 0; ch < 5; ch++) st.key[ch] = (data >> ch) & 1;
}

void scc_write(uint8_t port, uint8_t reg, uint8_t data) {
    switch (port) {
        case 2: waveform_write(reg, data); break;
        case 3: freq_write(reg, data); break;
        case 4: volume_write(reg, data); break;
        case 5: keyon_write(data); break;
        default: break;
    }
}

int16_t scc_render(void) {
    int32_t mix = 0;
#if SCC_VOLUME_SLEW_HZ > 0
    for (int ch = 0; ch < 5; ch++) {
        bool on = st.key[ch] && st.freq[ch] > 8;
        if (on) st.phase[ch] += st.step[ch]; // frozen (last) phase while off
        uint8_t idx = (uint8_t)((st.phase[ch] >> 16) & 0x1F);
        float target = on ? (float)st.volume[ch] : 0.0f;
        st.vol_smooth[ch] += st.vol_alpha * (target - st.vol_smooth[ch]);
        mix += (int32_t)((float)st.wave[ch][idx] * st.vol_smooth[ch]);
    }
#else
    for (int ch = 0; ch < 5; ch++) {
        if (!st.key[ch] || st.freq[ch] <= 8) continue;
        st.phase[ch] += st.step[ch];
        uint8_t idx = (uint8_t)((st.phase[ch] >> 16) & 0x1F);
        mix += (int32_t)st.wave[ch][idx] * st.volume[ch];
    }
#endif
    // wave is int8 (-128..127), volume 0-15 -> per-channel range ~+-1920;
    // >>3 keeps 5 simultaneously loud channels within audio_pwm's headroom.
    mix >>= 3;
    if (mix > 2047) mix = 2047;
    if (mix < -2048) mix = -2048;
    return (int16_t)mix;
}

uint32_t scc_sample_rate_hz(uint8_t clock_preset) {
    (void)clock_preset;
    return SCC_SAMPLE_RATE_HZ;
}
