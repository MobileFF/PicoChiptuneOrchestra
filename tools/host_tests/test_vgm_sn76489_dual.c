// Verifies master/src/vgm_player.c for the VGM spec's "Dual Chip Support" as
// applied to SN76489 (the only chip this project implements a second
// instance of -- see docs/circuit.md and vgm_chips.h):
//  * header_chip_mask() (via vgm_player_scan_chips()) sets VGM_CHIP_SN76489
//    AND VGM_CHIP_SN76489_2 when the header's SN76489 clock field (0x0C) has
//    bit 30 (0x40000000) set -- this also regression-tests hdr_clock()'s
//    masking: before it correctly cleared bits 30/31, a dual-chip clock
//    value overflowed the sanity ceiling and made even chip #1 disappear
//    from the mask.
//  * command 0x50 dd dispatches to VGM_CHIP_SN76489, and 0x30 dd (same wire
//    format) dispatches to VGM_CHIP_SN76489_2.
//  * VGM_CHIP_SN76489_2 is RESET with the SAME clock preset as
//    VGM_CHIP_SN76489 (the header has only one clock field for both).
//
// Builds its own recording slave_bus (instead of stub_slave_bus.c) so it can
// assert on what was dispatched. Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../src/protocol
//   test_vgm_sn76489_dual.c ../../src/master/src/vgm_player.c
//   ../../src/master/src/vgm_chips.c -o /tmp/sn76489_dual_test
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "slave_bus.h"
#include "vgm_player.h"

typedef struct { vgm_chip_id_t chip; uint8_t port, reg, data; } write_ev_t;
#define MAX_WRITES 64
static write_ev_t g_writes[MAX_WRITES];
static int g_nwrites = 0;

static uint8_t g_reset_preset[VGM_CHIP_COUNT];
static bool    g_reset_seen[VGM_CHIP_COUNT];

void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) { (void)c; return true; }
void slave_bus_reset(vgm_chip_id_t c, uint8_t preset) {
    if (c < VGM_CHIP_COUNT) { g_reset_preset[c] = preset; g_reset_seen[c] = true; }
}
void slave_bus_write(vgm_chip_id_t c, uint8_t port, uint8_t reg, uint8_t data) {
    if (g_nwrites < MAX_WRITES) g_writes[g_nwrites++] = (write_ev_t){c, port, reg, data};
}
void slave_bus_mute_all(void) {}
void slave_bus_set_clock(vgm_chip_id_t c, uint8_t p) { (void)c; (void)p; }
void slave_bus_send(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) { (void)c; (void)op; (void)r; (void)d; }
void slave_bus_send_burst(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) { (void)c; (void)op; (void)r; (void)d; }

static void put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Build a minimal VGM: version 1.61, SN76489 clock = `clock_field` (raw,
// dual-chip bit included if the caller sets it), data at 0x40:
//   0x50 0xAA  (chip 1 write)
//   0x30 0xBB  (chip 2 write)
//   0x66       (end)
static void write_vgm(const char *path, uint32_t clock_field) {
    uint8_t vgm[0x40 + 3 + 1];
    memset(vgm, 0, sizeof(vgm));
    memcpy(vgm, "Vgm ", 4);
    put_u32(vgm + 0x08, 0x00000161); // version
    put_u32(vgm + 0x0C, clock_field); // SN76489 clock (+ dual-chip flag)
    put_u32(vgm + 0x18, 2);          // total samples (unused by these asserts)

    uint8_t *d = vgm + 0x40;
    *d++ = 0x50; *d++ = 0xAA; // chip 1
    *d++ = 0x30; *d++ = 0xBB; // chip 2
    *d++ = 0x66;

    put_u32(vgm + 0x04, (uint32_t)(sizeof(vgm) - 4)); // EOF offset, relative to itself

    FILE *f = fopen(path, "wb");
    fwrite(vgm, 1, sizeof(vgm), f);
    fclose(f);
}

int main(void) {
    int fail = 0;
    #define CHK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); fail = 1; } } while (0)

    const char *path = "/tmp/sn76489_dual_test.vgm";
    const uint32_t REAL_CLOCK = 3579545;

    // --- Case A: dual-chip bit (30) set -------------------------------------
    write_vgm(path, REAL_CLOCK | 0x40000000u);

    uint32_t mask = 0;
    CHK(vgm_player_scan_chips(path, &mask));
    CHK((mask & (1u << VGM_CHIP_SN76489)) != 0);   // chip 1 still detected --
                                                    // regression check for
                                                    // hdr_clock()'s bit30/31 mask
    CHK((mask & (1u << VGM_CHIP_SN76489_2)) != 0); // dual-chip flag -> chip 2 too

    g_nwrites = 0;
    memset(g_reset_seen, 0, sizeof(g_reset_seen));
    vgm_player_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.loop_enabled = false;
    opts.max_loops = 0;
    CHK(vgm_player_play(path, &opts));

    CHK(g_nwrites == 2);
    if (g_nwrites == 2) {
        CHK(g_writes[0].chip == VGM_CHIP_SN76489 && g_writes[0].data == 0xAA);
        CHK(g_writes[1].chip == VGM_CHIP_SN76489_2 && g_writes[1].data == 0xBB);
    }
    // Only one clock field in the header for both instances -- chip 2 must
    // be reset with the exact same preset as chip 1.
    CHK(g_reset_seen[VGM_CHIP_SN76489] && g_reset_seen[VGM_CHIP_SN76489_2]);
    CHK(g_reset_preset[VGM_CHIP_SN76489] == g_reset_preset[VGM_CHIP_SN76489_2]);

    // --- Case B: dual-chip bit NOT set --------------------------------------
    // header_chip_mask() shouldn't claim a second chip exists, but a 0x30 in
    // the stream (nonstandard without the flag, but not impossible) is still
    // dispatched -- the dispatch itself never depends on the header flag.
    write_vgm(path, REAL_CLOCK);
    mask = 0;
    CHK(vgm_player_scan_chips(path, &mask));
    CHK((mask & (1u << VGM_CHIP_SN76489)) != 0);
    CHK((mask & (1u << VGM_CHIP_SN76489_2)) == 0);

    g_nwrites = 0;
    CHK(vgm_player_play(path, &opts));
    CHK(g_nwrites == 2);
    if (g_nwrites == 2) CHK(g_writes[1].chip == VGM_CHIP_SN76489_2 && g_writes[1].data == 0xBB);

    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
