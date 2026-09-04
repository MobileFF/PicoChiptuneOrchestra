// Offline VGM -> WAV renderer for the YM2151 (OPM) path.
//
// Same idea as render_wav.c (AY+SCC), but for the ymfm-OPM chip: it links
// the *shipped* master parser (src/master/src/vgm_player.c) and the *shipped*
// YM2151 wrapper (src/slave_ym2151/src/chip_ym2151.cpp -> third_party/ymfm),
// with perfect timing and none of the real-hardware path -- no SPI, no
// per-byte CS gap / burst, no inter-core FIFO, no PWM, no analog mixer.
//
// So an A/B of these WAVs against what the hardware plays isolates whatever
// the hardware path adds; an A/B against a reference OPM render (MAME /
// Nuked-OPM via libvgm) isolates our emulation itself.
//
// It renders at the YM2151's NATIVE sample rate for this song's clock
// (ymfm's ym2151::sample_rate(clock) -- 62500 Hz at 4 MHz, 55930 Hz at
// 3.579545 MHz), which is exactly what the slave's audio engine clocks
// ym2151_render() at. The WAV is written at that rate.
//
// Three stems, to localise where any grit/distortion enters:
//   _raw.wav     -- (L+R) straight from ymfm, before any scaling. The core.
//   _shipped.wav -- after chip_ym2151.cpp's `>>6` + clamp to +-2048 (12-bit).
//   _pwm10.wav   -- after a further `>>2` (what the slave's 10-bit PWM DAC
//                   actually reproduces; audio_pwm_write does v>>2).
// Each is peak-normalised to -1 dBFS so faint output is audible and the
// three are level-matched for ear comparison. Clip count + effective bit
// depth of the raw signal are printed.
//
// Build + run: tools/offline_render/run_ym2151.sh <file.vgm> <out_prefix> [seconds]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "pico/stdlib.h" // absolute_time_t + the sleep_until() we implement
#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"
}
#include "chip_ym2151.h" // has its own extern "C"

static uint32_t g_sr = 44100;   // set from the YM2151 clock preset on reset
static double   g_secs = 60.0;
static int64_t *g_raw;          // per-sample (L+R), pre-scale
static size_t   g_cap;
static size_t   g_done;
static int      g_on;
static uint64_t g_clip;         // samples the shipped >>6 path clamped
static int64_t  g_raw_peak = 1;

static void ensure_bufs(void) {
    if (g_raw) return;
    g_cap = (size_t)(g_secs * g_sr);
    g_raw = (int64_t *)calloc(g_cap, sizeof *g_raw);
    if (!g_raw) { fprintf(stderr, "oom (%zu samples)\n", g_cap); exit(1); }
}

static void render_to(uint64_t target) {
    ensure_bufs();
    if (target > g_cap) target = g_cap;
    while (g_done < target) {
        int32_t l = 0, r = 0;
        if (g_on) ym2151_render_raw(&l, &r);
        int64_t s = (int64_t)l + r;
        g_raw[g_done++] = s;
        int64_t a = s < 0 ? -s : s;
        if (a > g_raw_peak) g_raw_peak = a;
        int32_t m = (int32_t)(s >> 6);
        if (m > 2047 || m < -2048) g_clip++;
    }
}

// --- pico/stdlib.h hook: this is the render clock ---------------------------
extern "C" void sleep_until(absolute_time_t t_us) {
    render_to((uint64_t)t_us * g_sr / 1000000ull);
}

// --- slave_bus.h implementation: feed only the YM2151 ----------------------
extern "C" void slave_bus_init(void) {}
extern "C" bool slave_bus_has_chip(vgm_chip_id_t c) { return c == VGM_CHIP_YM2151; }
extern "C" void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c != VGM_CHIP_YM2151) return;
    ym2151_reset(preset);
    g_sr = ym2151_sample_rate_hz(preset);
    g_on = 1;
    printf("YM2151 reset: clock preset %u -> render %u Hz\n", preset, g_sr);
}
extern "C" void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    if (c == VGM_CHIP_YM2151) ym2151_write(port, reg, data);
}
extern "C" void slave_bus_set_clock(vgm_chip_id_t c, uint8_t preset) {
    (void)c; (void)preset; // native rate already fixed by slave_bus_reset()
}
extern "C" void slave_bus_send(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    (void)c; (void)opcode; (void)reg; (void)data; // YM2151 uses only WRITE0
}
extern "C" void slave_bus_mute_all(void) {}

// --- WAV out --------------------------------------------------------------
static void wr_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v>>8, f); fputc(v>>16, f); fputc(v>>24, f); }
static void wr_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v>>8, f); }

// Write `n` samples of `stem` (already reduced to mono int32) as 16-bit PCM
// at g_sr, peak-normalised to -1 dBFS.
static void write_wav(const char *path, const int32_t *stem, size_t n) {
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) { int32_t a = stem[i] < 0 ? -stem[i] : stem[i]; if (a > peak) peak = a; }
    double g = 29204.0 / (double)peak;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16); wr_u16(f, 1); wr_u16(f, 1);
    wr_u32(f, g_sr); wr_u32(f, g_sr * 2); wr_u16(f, 2); wr_u16(f, 16);
    fwrite("data", 1, 4, f); wr_u32(f, bytes);
    for (size_t i = 0; i < n; i++) {
        double v = stem[i] * g;
        int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        wr_u16(f, (uint16_t)(int16_t)s);
    }
    fclose(f);
    printf("wrote %s  (%.2fs @ %u Hz, peak %d -> gain x%.2f)\n",
           path, (double)n / g_sr, g_sr, peak, g);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.vgm> <out_prefix> [seconds=60] [shift=6]\n", argv[0]);
        return 2;
    }
    if (argc >= 4) g_secs = atof(argv[3]);
    int shift = argc >= 5 ? atoi(argv[4]) : 6; // chip_ym2151.cpp uses >>6

    vgm_player_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.loop_enabled = false;
    opts.max_loops = 1;
    if (!vgm_player_play(argv[1], &opts)) {
        fprintf(stderr, "vgm_player_play failed for %s\n", argv[1]);
        return 1;
    }

    size_t n = g_done;
    if (!n) { fprintf(stderr, "no samples rendered (YM2151 not used by this file?)\n"); return 1; }

    double rms = 0;
    for (size_t i = 0; i < n; i++) rms += (double)g_raw[i] * (double)g_raw[i];
    rms = sqrt(rms / n);
    printf("rendered %zu samples (%.2fs) @ %u Hz\n", n, (double)n / g_sr, g_sr);
    printf("raw (L+R): peak %lld  rms %.0f  effective bits ~%.1f  |  shipped >>6 clamped %llu/%zu samples (%.3f%%)\n",
           (long long)g_raw_peak, rms, log2((double)g_raw_peak + 1.0),
           (unsigned long long)g_clip, n, 100.0 * (double)g_clip / (double)n);

    int32_t *buf = (int32_t *)malloc(n * sizeof *buf);
    char p[512];

    for (size_t i = 0; i < n; i++) buf[i] = (int32_t)g_raw[i];
    snprintf(p, sizeof p, "%s_raw.wav", argv[2]); write_wav(p, buf, n);

    uint64_t clip_s = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t m = (int32_t)(g_raw[i] >> shift);
        if (m > 2047) { m = 2047; clip_s++; } else if (m < -2048) { m = -2048; clip_s++; }
        buf[i] = m;
    }
    printf("shipped path >>%d: clamped %llu/%zu samples (%.3f%%)\n",
           shift, (unsigned long long)clip_s, n, 100.0 * (double)clip_s / (double)n);
    snprintf(p, sizeof p, "%s_shipped.wav", argv[2]); write_wav(p, buf, n);

    for (size_t i = 0; i < n; i++) {
        int32_t m = (int32_t)(g_raw[i] >> shift);
        if (m > 2047) m = 2047; else if (m < -2048) m = -2048;
        buf[i] = m >> 2; // slave audio_pwm_write: 12-bit -> 10-bit
    }
    snprintf(p, sizeof p, "%s_pwm10.wav", argv[2]); write_wav(p, buf, n);

    free(buf);
    return 0;
}
