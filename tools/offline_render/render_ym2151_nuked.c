// Offline VGM -> WAV renderer for the YM2151 (OPM) path, using Nuked-OPM
// (cycle-accurate) as a REFERENCE core.
//
// Companion to render_ym2151.cpp, which uses this project's shipped ymfm-OPM.
// Same shipped master parser (src/master/src/vgm_player.c) drives the parse
// and timing; only the synthesis core differs. A/B the _raw WAVs from the
// two to isolate whether our ymfm usage is what colours the sound.
//
// Nuked-OPM is LGPL-2.1 and lives under tools/offline_render/vendor/ (git-
// ignored, host-only, never in firmware) -- see vendor/README.md.
//
// Nuked-OPM is a pin-level model: OPM_Clock() advances ONE internal cycle
// (chip->cycles wraps mod 32); the chip emits one stereo output sample every
// 32 internal cycles, i.e. at input_clock/64 -- 62500 Hz at a 4 MHz clock,
// which is exactly the rate render_ym2151.cpp / the slave use. A register
// write needs the chip clocked BETWEEN the address and data bytes (they
// share chip->write_data and the address must latch first), so we clock a
// small gap around each write and let the following wait absorb it (the
// cycle budget is tracked absolutely, so this self-compensates and the WAV
// stays the same length as the ymfm render).
//
// Build + run: tools/offline_render/run_ym2151_nuked.sh <file.vgm> <out_prefix> [seconds]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"
#include "opm.h"

// YM2151 clock presets -- MUST match src/master/src/vgm_player.c and
// src/slave_ym2151/src/chip_ym2151.cpp.
static const uint32_t CLOCK_PRESETS[2] = {3579545, 4000000};

#define CYC_PER_SAMPLE 32          // OPM_Clock calls per stereo output sample
#define WRITE_GAP_CYC  32          // clocks inserted before and after each reg write

static opm_t     s_chip;
static uint32_t  s_sr = 44100;
static double    s_secs = 60.0;
static int       s_on = 0;

static int64_t  *s_raw;            // per-sample (L+R), pre-scale
static size_t    s_cap;
static size_t    s_done;           // samples emitted
static uint64_t  s_cycles;         // total OPM_Clock calls
static uint32_t  s_cyc_in_frame;   // 0..CYC_PER_SAMPLE-1
static int64_t   s_raw_peak = 1;
static uint64_t  s_clip;           // shipped >>6 path would clamp

static void ensure_bufs(void) {
    if (s_raw) return;
    s_cap = (size_t)(s_secs * s_sr);
    s_raw = calloc(s_cap, sizeof *s_raw);
    if (!s_raw) { fprintf(stderr, "oom (%zu samples)\n", s_cap); exit(1); }
}

// Advance the chip by `n` internal cycles, emitting an output sample every
// CYC_PER_SAMPLE cycles.
static void run_cycles(uint64_t n) {
    ensure_bufs();
    for (uint64_t i = 0; i < n; i++) {
        int32_t o[2] = {0, 0};
        OPM_Clock(&s_chip, o, NULL, NULL, NULL);
        s_cycles++;
        if (++s_cyc_in_frame == CYC_PER_SAMPLE) {
            s_cyc_in_frame = 0;
            if (s_done < s_cap) {
                int64_t s = (int64_t)o[0] + o[1];
                s_raw[s_done++] = s;
                int64_t a = s < 0 ? -s : s;
                if (a > s_raw_peak) s_raw_peak = a;
                int32_t m = (int32_t)(s >> 6);
                if (m > 2047 || m < -2048) s_clip++;
            }
        }
    }
}

// Run the chip forward until total emitted cycles reach `target_cycles`.
static void run_until(uint64_t target_cycles) {
    if (target_cycles > s_cycles) run_cycles(target_cycles - s_cycles);
}

// --- pico/stdlib.h hook: render clock -------------------------------------
void sleep_until(absolute_time_t t_us) {
    // sample index at this song time, then the cycle count that reaches it
    uint64_t target_sample = (uint64_t)t_us * s_sr / 1000000ull;
    run_until(target_sample * CYC_PER_SAMPLE);
}

// --- slave_bus.h implementation: feed only the YM2151 -------------------
void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) { return c == VGM_CHIP_YM2151; }
void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c != VGM_CHIP_YM2151) return;
    OPM_Reset(&s_chip, opm_flags_none);
    uint32_t clk = CLOCK_PRESETS[preset < 2 ? preset : 0];
    s_sr = clk / 64;
    s_on = 1;
    printf("Nuked-OPM reset: clock preset %u (%u Hz) -> render %u Hz\n", preset, clk, s_sr);
}
void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    if (c != VGM_CHIP_YM2151 || !s_on) return;
    (void)port; // master always sends port 0 for YM2151
    OPM_Write(&s_chip, 0, reg);
    run_cycles(WRITE_GAP_CYC);
    OPM_Write(&s_chip, 1, data);
    run_cycles(WRITE_GAP_CYC);
}
void slave_bus_set_clock(vgm_chip_id_t c, uint8_t preset) {
    (void)c; (void)preset; // native rate already fixed by slave_bus_reset()
}
void slave_bus_send(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    (void)c; (void)opcode; (void)reg; (void)data;
}
void slave_bus_mute_all(void) {}

// --- WAV out -----------------------------------------------------------------
static void wr_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v>>8, f); fputc(v>>16, f); fputc(v>>24, f); }
static void wr_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v>>8, f); }

static void write_wav(const char *path, const int32_t *stem, size_t n) {
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) { int32_t a = stem[i] < 0 ? -stem[i] : stem[i]; if (a > peak) peak = a; }
    double g = 29204.0 / (double)peak;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16); wr_u16(f, 1); wr_u16(f, 1);
    wr_u32(f, s_sr); wr_u32(f, s_sr * 2); wr_u16(f, 2); wr_u16(f, 16);
    fwrite("data", 1, 4, f); wr_u32(f, bytes);
    for (size_t i = 0; i < n; i++) {
        double v = stem[i] * g;
        int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        wr_u16(f, (uint16_t)(int16_t)s);
    }
    fclose(f);
    printf("wrote %s  (%.2fs @ %u Hz, peak %d -> gain x%.2f)\n",
           path, (double)n / s_sr, s_sr, peak, g);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.vgm> <out_prefix> [seconds=60]\n", argv[0]);
        return 2;
    }
    if (argc >= 4) s_secs = atof(argv[3]);

    vgm_player_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.loop_enabled = false;
    opts.max_loops = 1;
    if (!vgm_player_play(argv[1], &opts)) {
        fprintf(stderr, "vgm_player_play failed for %s\n", argv[1]);
        return 1;
    }

    size_t n = s_done;
    if (!n) { fprintf(stderr, "no samples rendered (YM2151 not used?)\n"); return 1; }

    double rms = 0;
    for (size_t i = 0; i < n; i++) rms += (double)s_raw[i] * (double)s_raw[i];
    rms = sqrt(rms / n);
    printf("rendered %zu samples (%.2fs) @ %u Hz\n", n, (double)n / s_sr, s_sr);
    printf("raw (L+R): peak %lld  rms %.0f  effective bits ~%.1f  |  >>6 would clamp %llu/%zu (%.3f%%)\n",
           (long long)s_raw_peak, rms, log2((double)s_raw_peak + 1.0),
           (unsigned long long)s_clip, n, 100.0 * (double)s_clip / (double)n);

    int32_t *buf = malloc(n * sizeof *buf);
    char p[512];

    for (size_t i = 0; i < n; i++) buf[i] = (int32_t)s_raw[i];
    snprintf(p, sizeof p, "%s_nuked_raw.wav", argv[2]); write_wav(p, buf, n);

    for (size_t i = 0; i < n; i++) {
        int32_t m = (int32_t)(s_raw[i] >> 6);
        if (m > 2047) m = 2047; else if (m < -2048) m = -2048;
        buf[i] = m;
    }
    snprintf(p, sizeof p, "%s_nuked_shipped.wav", argv[2]); write_wav(p, buf, n);

    free(buf);
    return 0;
}
