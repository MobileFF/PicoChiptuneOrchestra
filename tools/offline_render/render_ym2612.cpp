// Offline VGM -> WAV renderer for the YM2612 (OPN2 / Mega Drive FM) path.
//
// Same idea as render_ym2151.cpp, for the ymfm-OPN2 chip: links the
// *shipped* master parser (src/master/src/vgm_player.c) and the *shipped*
// YM2612 wrapper (src/slave_ym2612/src/chip_ym2612.cpp -> third_party/ymfm),
// including the shipped Mega Drive "PCM" (DAC) checkpoint protocol -- with
// perfect timing and none of the real-hardware path (no SPI, no per-byte CS
// gap/burst, no inter-core FIFO, no PWM, no analog mixer, and every
// PCM_RESET/PCM_BYTE/DAC_START/DAC_SYNC/DAC_STOP/DAC_SEEK/DAC_RATE frame
// delivered exactly once, never dropped or duplicated).
//
// So an A/B of these WAVs against what the hardware plays isolates whatever
// the hardware path (SPI framing/timing, DAC checkpoint jitter, PWM output)
// adds; a render that's ALREADY noisy/clicky with nothing dropped points at
// either the VGM's own content (real-hardware-accurate low-fi FM/DAC) or our
// ymfm usage, not at SPI transport timing.
//
// Renders at the YM2612's NATIVE sample rate for this song's clock (ymfm's
// ym2612::sample_rate(clock) -- 53267 Hz at the NTSC 7670454 Hz preset),
// exactly what the slave's audio engine clocks ym2612_render() at.
//
// Two stems, to localise where any grit/distortion enters:
//   _raw.wav     -- (L+R) straight from ymfm, before any scaling. The core
//                   FM+DAC synthesis, unaffected by anything this project's
//                   transport/output path adds.
//   _shipped.wav -- after chip_ym2612.cpp's `>>3` + clamp to +-2048 (12-bit),
//                   i.e. exactly what the real slave board's audio_pwm sees.
// Each is peak-normalised to -1 dBFS so faint output is audible and the two
// are level-matched for ear comparison. Clip count + effective bit depth of
// the raw signal are printed, plus a simple click detector: the count of
// sample-to-sample jumps bigger than a threshold (see CLICK_JUMP_THRESHOLD),
// both on the raw and shipped stems, as a numeric proxy for "crackle" you
// don't have to listen for by ear.
//
// Build + run: tools/offline_render/run_ym2612.sh <file.vgm> <out_prefix> [seconds]

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
#include "chip_ym2612.h" // has its own extern "C"

static uint32_t g_sr = 44100;   // set from the YM2612 clock preset on reset
static double   g_secs = 60.0;
static int64_t *g_raw;          // per-sample (L+R), pre-scale
static size_t   g_cap;
static size_t   g_done;
static int      g_on;
static uint64_t g_clip;         // samples the shipped >>3 path clamped
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
        if (g_on) ym2612_render_raw(&l, &r);
        int64_t s = (int64_t)l + r;
        g_raw[g_done++] = s;
        int64_t a = s < 0 ? -s : s;
        if (a > g_raw_peak) g_raw_peak = a;
        int32_t m = (int32_t)(s >> 3);
        if (m > 2047 || m < -2048) g_clip++;
    }
}

// --- pico/stdlib.h hook: this is the render clock ---------------------------
extern "C" void sleep_until(absolute_time_t t_us) {
    render_to((uint64_t)t_us * g_sr / 1000000ull);
}

// --- slave_bus.h implementation: feed only the YM2612 -----------------------
extern "C" void slave_bus_init(void) {}
extern "C" bool slave_bus_has_chip(vgm_chip_id_t c) { return c == VGM_CHIP_YM2612; }
extern "C" void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c != VGM_CHIP_YM2612) return;
    ym2612_reset(preset);
    g_sr = ym2612_sample_rate_hz(preset);
    g_on = 1;
    printf("YM2612 reset: clock preset %u -> render %u Hz\n", preset, g_sr);
}
extern "C" void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    if (c == VGM_CHIP_YM2612) ym2612_write(port, reg, data);
}
extern "C" void slave_bus_set_clock(vgm_chip_id_t c, uint8_t preset) {
    (void)c; (void)preset; // native rate already fixed by slave_bus_reset()
}
// vgm_player.c's own DAC/PCM helpers (ym_dac_* in vgm_player.c) send the
// YM2612_PCM_*/DAC_* opcodes through BOTH of these -- translate to the
// wrapper's port numbers exactly as slave_engine.c's real dispatcher does
// (see its VGMSPI_OP_YM2612_* case block), so this offline path exercises
// the identical checkpoint protocol the real slave runs, just with perfect,
// un-dropped delivery.
static void dispatch_ym2612_op(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    if (c != VGM_CHIP_YM2612) return;
    switch (opcode) {
        case VGMSPI_OP_YM2612_PCM_RESET: ym2612_write(20, reg, data); break;
        case VGMSPI_OP_YM2612_PCM_BYTE:  ym2612_write(21, reg, data); break;
        case VGMSPI_OP_YM2612_DAC_START: ym2612_write(22, reg, data); break;
        case VGMSPI_OP_YM2612_DAC_SYNC:  ym2612_write(23, reg, data); break;
        case VGMSPI_OP_YM2612_DAC_STOP:  ym2612_write(24, reg, data); break;
        case VGMSPI_OP_YM2612_DAC_SEEK:  ym2612_write(25, reg, data); break;
        case VGMSPI_OP_YM2612_DAC_RATE:  ym2612_write(26, reg, data); break;
        default: break; // not a YM2612 DAC/PCM opcode -- ignore
    }
}
extern "C" void slave_bus_send(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    dispatch_ym2612_op(c, opcode, reg, data);
}
extern "C" void slave_bus_send_burst(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) {
    dispatch_ym2612_op(c, op, r, d);
}
extern "C" void slave_bus_mute_all(void) {}

// --- simple click detector: count big sample-to-sample jumps ---------------
#define CLICK_JUMP_THRESHOLD_FRAC 0.15 // 15% of the stem's own peak, per-sample delta

static uint64_t count_clicks(const int32_t *stem, size_t n, int32_t peak) {
    int64_t thresh = (int64_t)(peak * CLICK_JUMP_THRESHOLD_FRAC);
    uint64_t clicks = 0;
    for (size_t i = 1; i < n; i++) {
        int64_t d = (int64_t)stem[i] - (int64_t)stem[i - 1];
        if (d < 0) d = -d;
        if (d > thresh) clicks++;
    }
    return clicks;
}

// --- WAV out --------------------------------------------------------------
static void wr_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v>>8, f); fputc(v>>16, f); fputc(v>>24, f); }
static void wr_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v>>8, f); }

// Write `n` samples of `stem` (already reduced to mono int32) as 16-bit PCM
// at g_sr, peak-normalised to -1 dBFS. Returns the pre-normalisation peak
// (for count_clicks()'s threshold, which should be on the UN-normalised data).
static int32_t write_wav(const char *path, const int32_t *stem, size_t n) {
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) { int32_t a = stem[i] < 0 ? -stem[i] : stem[i]; if (a > peak) peak = a; }
    double g = 29204.0 / (double)peak;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return peak; }
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
    return peak;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.vgm> <out_prefix> [seconds=60] [shift=3]\n", argv[0]);
        return 2;
    }
    if (argc >= 4) g_secs = atof(argv[3]);
    int shift = argc >= 5 ? atoi(argv[4]) : 3; // chip_ym2612.cpp uses >>3

    vgm_player_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.loop_enabled = false;
    opts.max_loops = 1;
    if (!vgm_player_play(argv[1], &opts)) {
        fprintf(stderr, "vgm_player_play failed for %s\n", argv[1]);
        return 1;
    }

    size_t n = g_done;
    if (!n) { fprintf(stderr, "no samples rendered (YM2612 not used by this file?)\n"); return 1; }

    double rms = 0;
    for (size_t i = 0; i < n; i++) rms += (double)g_raw[i] * (double)g_raw[i];
    rms = sqrt(rms / n);
    printf("rendered %zu samples (%.2fs) @ %u Hz\n", n, (double)n / g_sr, g_sr);
    printf("raw (L+R): peak %lld  rms %.0f  effective bits ~%.1f  |  shipped >>%d clamped %llu/%zu samples (%.3f%%)\n",
           (long long)g_raw_peak, rms, log2((double)g_raw_peak + 1.0), shift,
           (unsigned long long)g_clip, n, 100.0 * (double)g_clip / (double)n);

    int32_t *buf = (int32_t *)malloc(n * sizeof *buf);
    char p[512];

    for (size_t i = 0; i < n; i++) buf[i] = (int32_t)g_raw[i];
    snprintf(p, sizeof p, "%s_raw.wav", argv[2]);
    int32_t raw_peak = write_wav(p, buf, n);
    uint64_t raw_clicks = count_clicks(buf, n, raw_peak);

    uint64_t clip_s = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t m = (int32_t)(g_raw[i] >> shift);
        if (m > 2047) { m = 2047; clip_s++; } else if (m < -2048) { m = -2048; clip_s++; }
        buf[i] = m;
    }
    printf("shipped path >>%d: clamped %llu/%zu samples (%.3f%%)\n",
           shift, (unsigned long long)clip_s, n, 100.0 * (double)clip_s / (double)n);
    snprintf(p, sizeof p, "%s_shipped.wav", argv[2]);
    int32_t shipped_peak = write_wav(p, buf, n);
    uint64_t shipped_clicks = count_clicks(buf, n, shipped_peak);

    printf("\nclick detector (sample-to-sample jump > %.0f%% of that stem's own peak):\n",
           100.0 * CLICK_JUMP_THRESHOLD_FRAC);
    printf("  raw:      %llu jumps (%.2f/sec)\n",
           (unsigned long long)raw_clicks, raw_clicks / ((double)n / g_sr));
    printf("  shipped:  %llu jumps (%.2f/sec)\n",
           (unsigned long long)shipped_clicks, shipped_clicks / ((double)n / g_sr));
    printf("A similar rate on 'raw' (no SPI/transport in this render at all) means the FM+DAC\n"
           "content/synthesis itself is the source, not SPI timing or the real-hardware path.\n");

    free(buf);
    return 0;
}
