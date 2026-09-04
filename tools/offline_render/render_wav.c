// Offline VGM -> WAV renderer for the AY-3-8910 + K051649/SCC pair.
//
// Purpose: reproduce EXACTLY what this project's chip emulation would output
// for a given .vgm, with perfect timing and none of the real-hardware path
// (no SPI, no per-byte CS gap, no inter-core FIFO, no analog mixer). It links
// the *shipped* sources -- src/master/src/vgm_player.c drives the parse and
// pacing, src/slave_ay8910/src/chip_ay8910.c and src/slave_scc/src/chip_scc.c
// do the synthesis -- so an A/B against a hardware recording isolates whatever
// the hardware path adds, and an A/B against a real MSX / reference emulator
// isolates the emulation itself.
//
// vgm_player.c's wait_samples() calls sleep_until(delayed_by_us(song_start,
// target_us)); with song_start pinned at 0 (see shim/pico/stdlib.h) the
// argument is just "us since song start", which we convert to a sample count
// and render up to.
//
// Build + run: see tools/offline_render/run.sh (from the repo root).
//   render_wav "調査用/04 Hot Summer Riding.vgm" out_prefix [seconds] [ay_lpf_hz] [scc_lpf_hz]
// Writes out_prefix_mix.wav, out_prefix_ay.wav, out_prefix_scc.wav
// (44100 Hz, mono, 16-bit, each peak-normalised).
//
// ay_lpf_hz / scc_lpf_hz (optional, default 0 = off): a one-pole low-pass
// applied to that stem right after render(), before mixing/writing. This is
// a DSP-experiment knob -- see docs/design-notes.md "Hot Summer Riding"
// investigation: the raw emulator output attacks harder (sharper edges on
// every envelope/volume-step retrigger) than a reference recording of the
// real track, measured as ~1.5x the onset density in a busy passage. Sweep
// this to see what cutoff brings the onset density back down to ~1x before
// deciding whether/how to carry a similar filter into the real firmware.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h" // absolute_time_t + the sleep_until() we implement below
#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"
#include "chip_ay8910.h"
#include "chip_scc.h"

#define SR 44100

static int32_t *g_ay;   // per-sample AY stem (unity gain, int32 headroom)
static int32_t *g_scc;  // per-sample SCC stem
static size_t   g_cap;  // capacity in samples
static size_t   g_done; // samples rendered so far
static int      g_ay_on, g_scc_on;

typedef struct { double a, y; int on; } lpf_t; // fwd decl body below slave_bus block
static void lpf_init(lpf_t *f, double hz, double sr);
static int32_t lpf_step(lpf_t *f, int32_t x);
static lpf_t g_ay_lpf, g_scc_lpf;

static void render_to(uint64_t target) {
    if (target > g_cap) target = g_cap;
    while (g_done < target) {
        g_ay[g_done]  = lpf_step(&g_ay_lpf,  g_ay_on  ? ay8910_render() : 0);
        g_scc[g_done] = lpf_step(&g_scc_lpf, g_scc_on ? scc_render()    : 0);
        g_done++;
    }
}

// --- pico/stdlib.h hook: this is the render clock -----------------------
void sleep_until(absolute_time_t t_us) {
    render_to((uint64_t)t_us * SR / 1000000ull);
}

// --- slave_bus.h implementation: feed the emulators --------------------
void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) {
    return c == VGM_CHIP_AY8910 || c == VGM_CHIP_SCC;
}
void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c == VGM_CHIP_AY8910) { ay8910_reset(preset); g_ay_on = 1; }
    else if (c == VGM_CHIP_SCC) { scc_reset(preset); g_scc_on = 1; }
}
void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    (void)port;
    if (c == VGM_CHIP_AY8910) ay8910_write(0, reg, data);
}
void slave_bus_set_clock(vgm_chip_id_t c, uint8_t preset) { (void)c; (void)preset; } // rate already fixed by slave_bus_reset()
void slave_bus_send(vgm_chip_id_t c, uint8_t opcode, uint8_t reg, uint8_t data) {
    if (c != VGM_CHIP_SCC) return;
    switch (opcode) { // same mapping as slave_common/src/slave_engine.c
        case VGMSPI_OP_SCC_WAVEFORM: scc_write(2, reg, data); break;
        case VGMSPI_OP_SCC_FREQ:     scc_write(3, reg, data); break;
        case VGMSPI_OP_SCC_VOLUME:   scc_write(4, reg, data); break;
        case VGMSPI_OP_SCC_KEYON:    scc_write(5, reg, data); break;
        default: break;
    }
}
void slave_bus_mute_all(void) {}

// --- one-pole low-pass, applied per-stem as a post-render DSP experiment ---
// (struct + fwd decls are up by render_to(), which runs ahead of these defs)
static void lpf_init(lpf_t *f, double hz, double sr) {
    f->on = hz > 0.0;
    f->a = f->on ? 1.0 - exp(-2.0 * M_PI * hz / sr) : 1.0;
    f->y = 0.0;
}
static int32_t lpf_step(lpf_t *f, int32_t x) {
    if (!f->on) return x;
    f->y += f->a * ((double)x - f->y);
    return (int32_t)(f->y < 0 ? f->y - 0.5 : f->y + 0.5);
}

// --- WAV out ----------------------------------------------------------------
static void wr_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v>>8, f); fputc(v>>16, f); fputc(v>>24, f); }
static void wr_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v>>8, f); }

static void write_wav(const char *path, const int32_t *stem, size_t n) {
    // peak-normalise to -1 dBFS so faint stems are audible and levels are comparable
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) { int32_t a = stem[i] < 0 ? -stem[i] : stem[i]; if (a > peak) peak = a; }
    double g = 29204.0 / (double)peak; // 32767 * 10^(-1/20)

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f); wr_u32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr_u32(f, 16); wr_u16(f, 1); wr_u16(f, 1);
    wr_u32(f, SR); wr_u32(f, SR * 2); wr_u16(f, 2); wr_u16(f, 16);
    fwrite("data", 1, 4, f); wr_u32(f, bytes);
    for (size_t i = 0; i < n; i++) {
        double v = stem[i] * g;
        int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        wr_u16(f, (uint16_t)(int16_t)s);
    }
    fclose(f);
    printf("wrote %s  (%.2fs, peak %d -> gain x%.2f)\n", path, (double)n / SR, peak, g);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.vgm> <out_prefix> [seconds=40] [ay_lpf_hz=0] [scc_lpf_hz=0]\n", argv[0]);
        return 2;
    }
    double secs = argc >= 4 ? atof(argv[3]) : 40.0;
    double ay_lpf_hz  = argc >= 5 ? atof(argv[4]) : 0.0;
    double scc_lpf_hz = argc >= 6 ? atof(argv[5]) : 0.0;
    lpf_init(&g_ay_lpf, ay_lpf_hz, SR);
    lpf_init(&g_scc_lpf, scc_lpf_hz, SR);
    if (ay_lpf_hz > 0 || scc_lpf_hz > 0)
        printf("post-render LPF: AY %.0fHz  SCC %.0fHz\n", ay_lpf_hz, scc_lpf_hz);
    g_cap = (size_t)(secs * SR);
    g_ay = calloc(g_cap, sizeof *g_ay);
    g_scc = calloc(g_cap, sizeof *g_scc);
    if (!g_ay || !g_scc) { fprintf(stderr, "oom\n"); return 1; }

    vgm_player_opts_t opts = { .loop_enabled = false, .max_loops = 1 };
    if (!vgm_player_play(argv[1], &opts)) {
        fprintf(stderr, "vgm_player_play failed for %s\n", argv[1]);
        return 1;
    }
    printf("rendered %zu samples (%.2fs); AY=%d SCC=%d\n", g_done, (double)g_done / SR, g_ay_on, g_scc_on);

    size_t n = g_done;
    int32_t *mix = calloc(n, sizeof *mix);
    for (size_t i = 0; i < n; i++) mix[i] = g_ay[i] + g_scc[i];

    char p[512];
    snprintf(p, sizeof p, "%s_mix.wav", argv[2]); write_wav(p, mix, n);
    snprintf(p, sizeof p, "%s_ay.wav",  argv[2]); write_wav(p, g_ay, n);
    snprintf(p, sizeof p, "%s_scc.wav", argv[2]); write_wav(p, g_scc, n);
    return 0;
}
