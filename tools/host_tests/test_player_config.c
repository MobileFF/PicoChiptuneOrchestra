// Host test for master/src/player_config.c's INI parser (player_config_apply).
// Stubs the three slave_bus_set_* sinks and checks that sections, key
// aliases, name normalisation (case / - / _ / space), comments and bad
// values are all handled. Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../master/src  test_player_config.c
//   ../../master/src/player_config.c  -o /tmp/player_config_test
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "player_config.h"
#include "vgm_chips.h"
#include "slave_bus.h"

static int  g_present[VGM_CHIP_COUNT];
static int  g_cs[VGM_CHIP_COUNT];
static long g_gap[VGM_CHIP_COUNT];

void slave_bus_set_present(vgm_chip_id_t c, bool v)   { if (c < VGM_CHIP_COUNT) g_present[c] = v ? 1 : 0; }
void slave_bus_set_cs_gpio(vgm_chip_id_t c, unsigned v){ if (c < VGM_CHIP_COUNT) g_cs[c] = (int)v; }
void slave_bus_set_gap_us(vgm_chip_id_t c, uint32_t v) { if (c < VGM_CHIP_COUNT) g_gap[c] = (long)v; }

int main(void) {
    for (int i = 0; i < VGM_CHIP_COUNT; i++) { g_present[i] = -1; g_cs[i] = -1; g_gap[i] = -1; }

    const char *cfg =
        "; a comment line\n"
        "# another comment\n"
        "\n"
        "[sn76489]\n"
        "enabled = no\n"
        "\n"
        "[AY-3-8910]\n"          // normalises to AY8910
        "cs = 7   ; inline comment after the value\n"
        "GAP_US=55\n"
        "\n"
        "[Sega PCM]\n"           // normalises to SEGAPCM
        "ENABLED = TrUe\n"
        "Pin = 27\n"
        "\n"
        "[ym2203]\n"
        "enabled = maybe\n"      // bad boolean -> ignored, not counted
        "cs = twelve\n"          // bad number  -> ignored, not counted
        "wobble = 3\n"           // unknown key -> ignored
        "\n"
        "[bogus_chip]\n"         // unknown section -> its keys skipped
        "cs = 5\n"
        "\n"
        "[scc]\n"
        "cs = 28\n"              // boundary-valid GPIO
        "this line has no equals sign\n";

    int n = player_config_apply(cfg);

    int fail = 0;
    #define CHK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); fail = 1; } } while (0)

    CHK(g_present[VGM_CHIP_SN76489] == 0);          // enabled = no
    CHK(g_cs[VGM_CHIP_AY8910] == 7);                // [AY-3-8910] cs, inline comment stripped
    CHK(g_gap[VGM_CHIP_AY8910] == 55);              // GAP_US alias
    CHK(g_present[VGM_CHIP_SEGAPCM] == 1);          // [Sega PCM] ENABLED = TrUe
    CHK(g_cs[VGM_CHIP_SEGAPCM] == 27);              // "Pin" alias
    CHK(g_present[VGM_CHIP_YM2203] == -1);          // "maybe" rejected
    CHK(g_cs[VGM_CHIP_YM2203] == -1);               // "twelve" rejected
    CHK(g_cs[VGM_CHIP_SCC] == 28);
    CHK(g_present[VGM_CHIP_YM2612] == -1);          // never mentioned -> untouched
    CHK(g_cs[VGM_CHIP_YM2612] == -1);
    CHK(n == 6); // sn.enabled, ay.cs, ay.gap, segapcm.enabled, segapcm.cs, scc.cs

    printf("applied=%d\n", n);
    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
