// Offline VGM -> WAV renderer for the Sega PCM path (diagnostic, not shipped
// with the rest of offline_render's README yet).
//
// Same idea as render_wav.c / render_ym2151.cpp: link the *shipped* master
// parser (src/master/src/vgm_player.c) and the *shipped* Sega PCM emulator
// (src/slave_segapcm/src/chip_segapcm.c) with perfect timing and NONE of the
// real-hardware path -- no SPI, no per-byte CS gap/burst, no inter-core FIFO,
// no PWM, no analog mixer. If the "extra drum" artifact (Space Harrier /
// "02 Theme.vgm", 2026-09-19 investigation) still appears here, it's a bug in
// chip_segapcm.c's register-driven logic itself, deterministic and
// independent of the SPI transport (which two separate hardware experiments
// -- gap_us 40->120, then burst mode -- already failed to fix).
//
// Renders at the chip's own native rate for this song's clock preset
// (segapcm_sample_rate_hz() -- 31250 Hz at 4 MHz, 62500 Hz at 8 MHz).
// segapcm_render() already does this project's internal mix + >>4 + clamp,
// so the WAV here is exactly what real hardware would play, sample for
// sample, given perfect delivery.
//
// Build + run: tools/offline_render/run_segapcm.sh <file.vgm> <out_prefix> [seconds]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h" // absolute_time_t + the sleep_until() we implement
#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"
#include "chip_segapcm.h"

static uint32_t g_sr = 31250;   // set from the clock preset on RESET
static double   g_secs = 60.0;
static int32_t *g_raw;          // per-sample segapcm_render() output (already mixed+clamped)
static size_t   g_cap;
static size_t   g_done;
static int      g_on;
static int64_t  g_peak = 1;

static void ensure_bufs(void) {
    if (g_raw) return;
    g_cap = (size_t)(g_secs * g_sr);
    g_raw = (int32_t *)calloc(g_cap, sizeof *g_raw);
    if (!g_raw) { fprintf(stderr, "oom (%zu samples)\n", g_cap); exit(1); }
}

static void render_to(uint64_t target) {
    ensure_bufs();
    if (target > g_cap) target = g_cap;
    while (g_done < target) {
        int16_t s = g_on ? segapcm_render() : 0;
        g_raw[g_done++] = s;
        int64_t a = s < 0 ? -s : s;
        if (a > g_peak) g_peak = a;
    }
}

// --- pico/stdlib.h hook: this is the render clock ---------------------------
void sleep_until(absolute_time_t t_us) {
    render_to((uint64_t)t_us * g_sr / 1000000ull);
}

// --- slave_bus.h implementation: feed only Sega PCM -------------------------
void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) { return c == VGM_CHIP_SEGAPCM; }
void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c != VGM_CHIP_SEGAPCM) return;
    segapcm_reset(preset);
    g_sr = segapcm_sample_rate_hz(preset);
    g_on = 1;
    printf("SegaPCM reset: clock preset %u -> render %u Hz\n", preset, g_sr);
}
void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    if (c == VGM_CHIP_SEGAPCM) segapcm_write(port, reg, data);
}
void slave_bus_set_clock(vgm_chip_id_t c, uint8_t preset) {
    (void)c; (void)preset; // native rate already fixed by slave_bus_reset()
}
void slave_bus_send(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    if (c != VGM_CHIP_SEGAPCM) return;
    switch (opcode) { // same mapping as slave_common/src/slave_engine.c
        case VGMSPI_OP_PCM_UPLOAD_RESET: segapcm_write(6, reg, data); break;
        case VGMSPI_OP_PCM_UPLOAD_BYTE:  segapcm_write(7, reg, data); break;
        case VGMSPI_OP_SEGAPCM_BANK:     segapcm_write(8, reg, data); break;
        case VGMSPI_OP_SEGAPCM_ROM_BASE: segapcm_write(9, reg, data); break;
        default: break;
    }
}
void slave_bus_send_burst(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) { slave_bus_send(c, op, r, d); }
void slave_bus_mute_all(void) {}

// --- WAV out ----------------------------------------------------------------
static void wr_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v>>8, f); fputc(v>>16, f); fputc(v>>24, f); }
static void wr_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v>>8, f); }

static void write_wav(const char *path, const int32_t *stem, size_t n, uint32_t sr, int normalise) {
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) { int32_t a = stem[i] < 0 ? -stem[i] : stem[i]; if (a > peak) peak = a; }
    double g = normalise ? 29204.0 / (double)peak : 1.0; // 32767 * 10^(-1/20)
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16); wr_u16(f, 1); wr_u16(f, 1);
    wr_u32(f, sr); wr_u32(f, sr * 2); wr_u16(f, 2); wr_u16(f, 16);
    fwrite("data", 1, 4, f); wr_u32(f, bytes);
    for (size_t i = 0; i < n; i++) {
        double v = stem[i] * g;
        int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        wr_u16(f, (uint16_t)(int16_t)s);
    }
    fclose(f);
    printf("wrote %s  (%.2fs @ %u Hz, peak %d, gain x%.2f)\n",
           path, (double)n / sr, sr, peak, g);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.vgm> <out_prefix> [seconds=60]\n", argv[0]);
        return 2;
    }
    if (argc >= 4) g_secs = atof(argv[3]);

    vgm_player_opts_t opts;
    memset(&opts, 0, sizeof opts);
    opts.loop_enabled = false;
    opts.max_loops = 1;
    if (!vgm_player_play(argv[1], &opts)) {
        fprintf(stderr, "vgm_player_play failed for %s\n", argv[1]);
        return 1;
    }

    size_t n = g_done;
    if (!n) { fprintf(stderr, "no samples rendered (SegaPCM not used by this file?)\n"); return 1; }

    double rms = 0;
    for (size_t i = 0; i < n; i++) rms += (double)g_raw[i] * (double)g_raw[i];
    rms = sqrt(rms / n);
    printf("rendered %zu samples (%.2fs) @ %u Hz -- peak %lld rms %.0f\n",
           n, (double)n / g_sr, g_sr, (long long)g_peak, rms);

    char p[512];
    snprintf(p, sizeof p, "%s_shipped.wav", argv[2]);
    write_wav(p, g_raw, n, g_sr, /*normalise=*/1);

    return 0;
}
