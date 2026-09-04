// Verifies master/src/vgm_player.c for Sega PCM:
//  * handle_data_block() for ROM-image blocks (VGM 0x67 type 0x80): the
//    8-byte prefix (total ROM size + start address) is stripped, an
//    UPLOAD_RESET carrying the block's start address is sent, exactly
//    (size - 8) data bytes are streamed as UPLOAD_BYTE, and the parser
//    stays in stream sync (reaches the trailing 0x66).
//  * the bank config from VGM header 0x3C is decoded and forwarded once as
//    VGMSPI_OP_SEGAPCM_BANK before the upload.
//  * (case C) a song whose ROM block sits high in the address space makes
//    the master pre-scan and send VGMSPI_OP_SEGAPCM_ROM_BASE = lowest block
//    start rounded down to 64KB, before any UPLOAD_RESET.
//
// Builds its own recording slave_bus (instead of stub_slave_bus.c) so it
// can assert on what was dispatched. Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../master/src -I ../../protocol
//   test_vgm_segapcm_block.c ../../master/src/vgm_player.c
//   ../../master/src/vgm_chips.c -o /tmp/segapcm_block_test
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"

#define MAX_EV 4096
typedef struct { uint8_t op, reg, data; } ev_t;
static ev_t g_send[MAX_EV];
static int g_nsend = 0;

void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) { (void)c; return true; }
void slave_bus_reset(vgm_chip_id_t c, uint8_t p) { (void)c; (void)p; }
void slave_bus_write(vgm_chip_id_t c, uint8_t po, uint8_t r, uint8_t d) { (void)c; (void)po; (void)r; (void)d; }
void slave_bus_mute_all(void) {}
void slave_bus_send(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) {
    (void)c;
    if (g_nsend < MAX_EV) g_send[g_nsend++] = (ev_t){op, r, d};
}

static void put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static const uint32_t START_ADDR = 0x000200; // 256-aligned, non-zero
static const int NDATA = 20;

// Build + play a Sega PCM VGM with one type-0x80 block and interface reg
// `intf`; return via out-params.
static int run_case(uint32_t intf, int *n_reset, int *n_byte, int *seek_ok,
                    int *data_ok, int *n_bank, uint8_t *bank_shift, uint8_t *bank_mask) {
    g_nsend = 0;
    uint8_t data[NDATA];
    for (int i = 0; i < NDATA; i++) data[i] = (uint8_t)(0x10 + i);

    uint8_t vgm[0x40 + 7 + 8 + 64 + 1];
    memset(vgm, 0, sizeof(vgm));
    memcpy(vgm, "Vgm ", 4);
    put_u32(vgm + 0x08, 0x00000161); // version 1.61
    put_u32(vgm + 0x38, 4000000);    // Sega PCM clock
    put_u32(vgm + 0x3C, intf);       // Sega PCM interface (bank) register

    uint8_t *d = vgm + 0x40;
    *d++ = 0x67; *d++ = 0x66; *d++ = 0x80;
    put_u32(d, 8 + NDATA); d += 4;
    put_u32(d, 0x040000); d += 4;     // prefix: total ROM size
    put_u32(d, START_ADDR); d += 4;   // prefix: chunk start address
    memcpy(d, data, NDATA); d += NDATA;
    *d++ = 0x66;

    const char *path = "/tmp/segapcm_block_test.vgm";
    FILE *f = fopen(path, "wb");
    fwrite(vgm, 1, (size_t)(d - vgm), f);
    fclose(f);

    vgm_player_opts_t opts = {.loop_enabled = false, .max_loops = 0, .poll_skip = NULL};
    bool ok = vgm_player_play(path, &opts);

    *n_reset = *n_byte = *seek_ok = *n_bank = 0;
    *data_ok = 1;
    *bank_shift = *bank_mask = 0;
    int bi = 0;
    for (int i = 0; i < g_nsend; i++) {
        ev_t e = g_send[i];
        if (e.op == VGMSPI_OP_PCM_UPLOAD_RESET) {
            (*n_reset)++;
            if (e.reg == (uint8_t)(START_ADDR >> 16) && e.data == (uint8_t)(START_ADDR >> 8))
                *seek_ok = 1;
        } else if (e.op == VGMSPI_OP_PCM_UPLOAD_BYTE) {
            (*n_byte)++;
            if (bi < NDATA && e.data != data[bi]) *data_ok = 0;
            bi++;
        } else if (e.op == VGMSPI_OP_SEGAPCM_BANK) {
            (*n_bank)++;
            *bank_shift = e.reg;
            *bank_mask = e.data;
        }
    }
    return ok ? 1 : 0;
}

// Build + play a Sega PCM VGM with one type-0x80 block whose chunk starts at
// `chunk_start` (256-aligned). Records everything dispatched into g_send.
static int run_highrom(uint32_t chunk_start) {
    g_nsend = 0;
    uint8_t data[NDATA];
    for (int i = 0; i < NDATA; i++) data[i] = (uint8_t)(0x10 + i);

    uint8_t vgm[0x40 + 7 + 8 + 64 + 1];
    memset(vgm, 0, sizeof(vgm));
    memcpy(vgm, "Vgm ", 4);
    put_u32(vgm + 0x08, 0x00000161);
    put_u32(vgm + 0x38, 4000000);
    put_u32(vgm + 0x3C, 0);

    uint8_t *d = vgm + 0x40;
    *d++ = 0x67; *d++ = 0x66; *d++ = 0x80;
    put_u32(d, 8 + NDATA); d += 4;
    put_u32(d, 0x080000); d += 4;      // total ROM size = 512KB
    put_u32(d, chunk_start); d += 4;
    memcpy(d, data, NDATA); d += NDATA;
    *d++ = 0x66;

    const char *path = "/tmp/segapcm_highrom_test.vgm";
    FILE *f = fopen(path, "wb");
    fwrite(vgm, 1, (size_t)(d - vgm), f);
    fclose(f);

    vgm_player_opts_t opts = {.loop_enabled = false, .max_loops = 0, .poll_skip = NULL};
    return vgm_player_play(path, &opts) ? 1 : 0;
}

int main(void) {
    int fail = 0;
    int n_reset, n_byte, seek_ok, data_ok, n_bank;
    uint8_t bshift, bmask;

    // Case A: interface reg 0 -> master forwards (0,0), slave keeps defaults.
    int ok = run_case(0, &n_reset, &n_byte, &seek_ok, &data_ok, &n_bank, &bshift, &bmask);
    printf("case A intf=0: ok=%d reset=%d byte=%d bank=%d(shift=%u mask=0x%02X)\n",
           ok, n_reset, n_byte, n_bank, bshift, bmask);
    if (!ok) { printf("FAIL: parser lost sync\n"); fail = 1; }
    if (!seek_ok) { printf("FAIL: no UPLOAD_RESET seek to 0x%X\n", START_ADDR); fail = 1; }
    if (n_byte != NDATA) { printf("FAIL: expected %d UPLOAD_BYTE, got %d\n", NDATA, n_byte); fail = 1; }
    if (!data_ok) { printf("FAIL: uploaded bytes != block data (prefix not stripped?)\n"); fail = 1; }
    if (n_bank != 1 || bshift != 0 || bmask != 0) { printf("FAIL: expected one BANK(0,0)\n"); fail = 1; }

    // Case B: interface reg 0x00F8000D -> shift 0x0D, mask 0x70|0xF8 = 0xF8.
    ok = run_case(0x00F8000Du, &n_reset, &n_byte, &seek_ok, &data_ok, &n_bank, &bshift, &bmask);
    printf("case B intf=0x00F8000D: bank=%d(shift=%u mask=0x%02X)\n", n_bank, bshift, bmask);
    if (!ok || !seek_ok || n_byte != NDATA || !data_ok) { printf("FAIL: block handling regressed\n"); fail = 1; }
    if (n_bank != 1 || bshift != 0x0D || bmask != 0xF8) {
        printf("FAIL: expected BANK(shift=0x0D mask=0xF8), got shift=%u mask=0x%02X\n", bshift, bmask);
        fail = 1;
    }

    // Case C: chunk at ROM 0x040090 (NOT 256-aligned) ->
    //  - ROM_BASE = 0x040000 (64KB-aligned) sent before the first UPLOAD_RESET
    //  - UPLOAD_RESET still carries the absolute 256-aligned page (0x04,0x00)
    //  - then 0x90 pad bytes of 0x80 (byte-granular chunk start), then the data
    const uint32_t CHUNK = 0x040090;
    int okC = run_highrom(CHUNK);
    int rb_idx = -1, blk_reset_idx = -1;
    uint8_t rb_reg = 0, rb_data = 0, rst_reg = 0, rst_data = 0;
    for (int i = 0; i < g_nsend; i++) {
        if (g_send[i].op == VGMSPI_OP_SEGAPCM_ROM_BASE && rb_idx < 0) {
            rb_idx = i; rb_reg = g_send[i].reg; rb_data = g_send[i].data;
        }
        // The per-block UPLOAD_RESET is the one directly followed by an
        // UPLOAD_BYTE (the song-start rewind at (0,0) is followed by
        // SEGAPCM_BANK etc.).
        if (g_send[i].op == VGMSPI_OP_PCM_UPLOAD_RESET && blk_reset_idx < 0 &&
            i + 1 < g_nsend && g_send[i + 1].op == VGMSPI_OP_PCM_UPLOAD_BYTE) {
            blk_reset_idx = i; rst_reg = g_send[i].reg; rst_data = g_send[i].data;
        }
    }
    int first_reset_idx = blk_reset_idx;
    uint32_t rom_base = ((uint32_t)rb_reg << 16) | ((uint32_t)rb_data << 8);
    // Count pad (0x80) UPLOAD_BYTE frames right after the per-block
    // UPLOAD_RESET, then check the real data follows intact.
    int pad = 0, data_start_idx = -1;
    for (int i = blk_reset_idx + 1; i < g_nsend; i++) {
        if (g_send[i].op != VGMSPI_OP_PCM_UPLOAD_BYTE) { data_start_idx = i; break; }
        if (g_send[i].data == 0x80) { pad++; }
        else { data_start_idx = i; break; }
    }
    int data_intact = (data_start_idx >= 0 && data_start_idx + NDATA <= g_nsend);
    for (int i = 0; data_intact && i < NDATA; i++)
        if (g_send[data_start_idx + i].data != (uint8_t)(0x10 + i)) data_intact = 0;

    printf("case C highrom: ok=%d rom_base=0x%06X reset=(0x%02X,0x%02X) pad=%d data_intact=%d\n",
           okC, rom_base, rst_reg, rst_data, pad, data_intact);
    if (!okC) { printf("FAIL: parser lost sync on high-ROM song\n"); fail = 1; }
    if (rb_idx < 0 || rom_base != 0x040000) { printf("FAIL: ROM_BASE not 0x040000\n"); fail = 1; }
    if (rb_idx >= 0 && first_reset_idx >= 0 && rb_idx > first_reset_idx) {
        printf("FAIL: ROM_BASE sent after an UPLOAD_RESET\n"); fail = 1;
    }
    if (rst_reg != (uint8_t)(CHUNK >> 16) || rst_data != (uint8_t)(CHUNK >> 8)) {
        printf("FAIL: UPLOAD_RESET should carry the 256-aligned page\n"); fail = 1;
    }
    if (pad != (int)(CHUNK & 0xFF)) { printf("FAIL: expected %d pad bytes, got %d\n", (int)(CHUNK & 0xFF), pad); fail = 1; }
    if (!data_intact) { printf("FAIL: real data not intact after the pad\n"); fail = 1; }

    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
