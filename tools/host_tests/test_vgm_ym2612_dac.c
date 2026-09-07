// Verifies master/src/vgm_player.c's YM2612 PCM / DAC streaming (Mega Drive
// "PCM"): the 0x67 type-0 data block is uploaded byte-for-byte as
// VGMSPI_OP_YM2612_PCM_BYTE, a run of VGM 0x8n commands emits
// DAC_START + DAC_RATE (and re-checks the rate over the run), a 0xE0 seek
// re-anchors the next run, any non-0x8n command ends a run with DAC_STOP,
// and the parser stays in stream sync throughout (reaches the trailing 0x66).
//
// Self-contained (builds its own recording slave_bus). Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../src/protocol  test_vgm_ym2612_dac.c ../../src/master/src/vgm_player.c ../../src/master/src/vgm_chips.c -o /tmp/ym2612_dac_test
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "slave_bus.h"
#include "vgm_player.h"
#include "vgm_spi_protocol.h"

#define MAX_EV 8192
typedef struct { uint8_t op, reg, data; } ev_t;
static ev_t g_send[MAX_EV];
static int g_nsend = 0;
typedef struct { uint8_t chip, port, reg, data; } wr_t;
static wr_t g_write[MAX_EV];
static int g_nwrite = 0;

void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t c) { (void)c; return true; }
void slave_bus_reset(vgm_chip_id_t c, uint8_t p) { (void)c; (void)p; }
void slave_bus_set_clock(vgm_chip_id_t c, uint8_t p) { (void)c; (void)p; }
void slave_bus_mute_all(void) {}
void slave_bus_write(vgm_chip_id_t c, uint8_t po, uint8_t r, uint8_t d) {
    if (g_nwrite < MAX_EV) g_write[g_nwrite++] = (wr_t){(uint8_t)c, po, r, d};
}
void slave_bus_send(vgm_chip_id_t c, uint8_t op, uint8_t r, uint8_t d) {
    (void)c;
    if (g_nsend < MAX_EV) g_send[g_nsend++] = (ev_t){op, r, d};
}

static void put_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

static int count_op(uint8_t op) {
    int n = 0;
    for (int i = 0; i < g_nsend; i++) if (g_send[i].op == op) n++;
    return n;
}
static int first_op(uint8_t op) {
    for (int i = 0; i < g_nsend; i++) if (g_send[i].op == op) return i;
    return -1;
}

int main(void) {
    static const int NPCM = 48;          // PCM bank bytes
    static const int RUN1 = 40;          // 0x81 commands in run 1
    static const int RUN2 = 10;          // 0x82 commands in run 2 (after a seek)
    static const uint32_t SEEK = 0x012345;

    uint8_t vgm[0x40 + 8 + NPCM + 3 + RUN1 + 5 + RUN2 + 1 + 1 + 64];
    memset(vgm, 0, sizeof(vgm));
    memcpy(vgm, "Vgm ", 4);
    put_u32(vgm + 0x08, 0x00000150);     // version 1.50
    put_u32(vgm + 0x2C, 7670454);        // YM2612 clock (NTSC)

    uint8_t *d = vgm + 0x40;
    // 0x67 type-0 PCM data block, NPCM bytes = 0x40,0x41,0x42,...
    *d++ = 0x67; *d++ = 0x66; *d++ = 0x00; put_u32(d, NPCM); d += 4;
    for (int i = 0; i < NPCM; i++) *d++ = (uint8_t)(0x40 + i);
    // enable DAC: YM2612 port 0, reg 0x2B, data 0x80  (a normal 0x52 write)
    *d++ = 0x52; *d++ = 0x2B; *d++ = 0x80;
    // run 1: RUN1 x 0x81  (DAC step + wait 1)
    for (int i = 0; i < RUN1; i++) *d++ = 0x81;
    // 0xE0 seek, then run 2: RUN2 x 0x82
    *d++ = 0xE0; put_u32(d, SEEK); d += 4;
    for (int i = 0; i < RUN2; i++) *d++ = 0x82;
    // a plain wait ends run 2, then end-of-stream
    *d++ = 0x62;
    *d++ = 0x66;

    const char *path = "/tmp/ym2612_dac_test.vgm";
    FILE *f = fopen(path, "wb");
    fwrite(vgm, 1, (size_t)(d - vgm), f);
    fclose(f);

    vgm_player_opts_t opts = {0}; // no loop, no skip
    bool ok = vgm_player_play(path, &opts);

    int fails = 0;
    #define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

    CHECK(ok, "vgm_player_play returned false");

    // --- PCM bank upload ---
    CHECK(count_op(VGMSPI_OP_YM2612_PCM_RESET) == 1, "expected exactly one PCM_RESET");
    int nbyte = count_op(VGMSPI_OP_YM2612_PCM_BYTE);
    CHECK(nbyte == NPCM, "PCM_BYTE count != NPCM");
    // the PCM_RESET must come before the first PCM_BYTE
    CHECK(first_op(VGMSPI_OP_YM2612_PCM_RESET) >= 0 &&
          first_op(VGMSPI_OP_YM2612_PCM_RESET) < first_op(VGMSPI_OP_YM2612_PCM_BYTE),
          "PCM_RESET not before first PCM_BYTE");
    // PCM_BYTE payload bytes, in order, are 0x40..0x40+NPCM-1
    { int k = 0;
      for (int i = 0; i < g_nsend && k < NPCM; i++)
          if (g_send[i].op == VGMSPI_OP_YM2612_PCM_BYTE) {
              if (g_send[i].data != (uint8_t)(0x40 + k)) { printf("FAIL: PCM_BYTE[%d]=0x%02X\n", k, g_send[i].data); fails++; break; }
              k++;
          }
    }

    // --- DAC control: two runs -> two DAC_START, at least one STOP each ---
    CHECK(count_op(VGMSPI_OP_YM2612_DAC_START) == 2, "expected 2 DAC_START (run1, run2)");
    CHECK(count_op(VGMSPI_OP_YM2612_DAC_STOP) >= 2, "expected >=2 DAC_STOP (0xE0 ends run1, 0x62 ends run2)");
    CHECK(count_op(VGMSPI_OP_YM2612_DAC_RATE) >= 2, "expected >=2 DAC_RATE");

    // run 1 starts at bank offset 0 (no prior seek)
    { int i = first_op(VGMSPI_OP_YM2612_DAC_START);
      CHECK(i >= 0 && g_send[i].reg == 0x00 && g_send[i].data == 0x00, "run1 DAC_START offset != 0"); }
    // run 2 starts at SEEK: seek_hi (0x01) then DAC_START reg:data = 0x2345
    { int seen_seek = 0, seen_start2 = 0, starts = 0;
      for (int i = 0; i < g_nsend; i++) {
          if (g_send[i].op == VGMSPI_OP_YM2612_DAC_SEEK && g_send[i].data == 0x01) seen_seek = 1;
          if (g_send[i].op == VGMSPI_OP_YM2612_DAC_START) {
              starts++;
              if (starts == 2 && g_send[i].reg == 0x23 && g_send[i].data == 0x45) seen_start2 = 1;
          }
      }
      CHECK(seen_seek, "no DAC_SEEK data=0x01 for the >64KB run-2 offset");
      CHECK(seen_start2, "run2 DAC_START offset != 0x2345");
    }

    // DAC enable write still forwarded normally
    { int seen = 0;
      for (int i = 0; i < g_nwrite; i++)
          if (g_write[i].reg == 0x2B && g_write[i].data == 0x80) seen = 1;
      CHECK(seen, "0x52 2B 80 (DAC enable) not forwarded as a normal write");
    }

    if (fails == 0) printf("ok (%d send events, %d writes)\n", g_nsend, g_nwrite);
    return fails ? 1 : 0;
}
