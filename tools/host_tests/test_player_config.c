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
static int  g_volume[VGM_CHIP_COUNT];

void slave_bus_set_present(vgm_chip_id_t c, bool v)   { if (c < VGM_CHIP_COUNT) g_present[c] = v ? 1 : 0; }
void slave_bus_set_cs_gpio(vgm_chip_id_t c, unsigned v){ if (c < VGM_CHIP_COUNT) g_cs[c] = (int)v; }
void slave_bus_set_gap_us(vgm_chip_id_t c, uint32_t v) { if (c < VGM_CHIP_COUNT) g_gap[c] = (long)v; }
void slave_bus_set_volume_pct(vgm_chip_id_t c, uint8_t v) { if (c < VGM_CHIP_COUNT) g_volume[c] = (int)v; }

int main(void) {
    for (int i = 0; i < VGM_CHIP_COUNT; i++) { g_present[i] = -1; g_cs[i] = -1; g_gap[i] = -1; g_volume[i] = -1; }

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
        "volume = 50\n"
        "\n"
        "[Sega PCM]\n"           // normalises to SEGAPCM
        "ENABLED = TrUe\n"
        "Pin = 27\n"
        "\n"
        "[ym2203]\n"
        "enabled = maybe\n"      // bad boolean -> ignored, not counted
        "cs = twelve\n"          // bad number  -> ignored, not counted
        "vol = 999\n"            // out of 0-255 range -> ignored, not counted
        "wobble = 3\n"           // unknown key -> ignored
        "\n"
        "[bogus_chip]\n"         // unknown section -> its keys skipped
        "cs = 5\n"
        "\n"
        "[scc]\n"
        "cs = 28\n"              // boundary-valid GPIO
        "this line has no equals sign\n"
        "\n"
        "[player]\n"             // not a chip -- general playback settings
        "shuffle = yes\n"
        "skip_button = 24\n"     // clone board's USR button, e.g.
        "preview = on\n"
        "preview_seconds = 15\n"
        "recursive = yes\n"
        "wobble = 3\n";          // unknown key in [player] -> ignored

    int fail = 0;
    #define CHK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); fail = 1; } } while (0)

    CHK(player_config_shuffle_enabled() == false); // default before parsing
    CHK(player_config_skip_button_gpio() == -1);   // default: not set
    CHK(player_config_preview_enabled() == false); // default before parsing
    CHK(player_config_preview_seconds() == 30);    // built-in default
    CHK(player_config_recursive_enabled() == false); // default before parsing
    int n = player_config_apply(cfg);
    CHK(player_config_shuffle_enabled() == true);   // [player] shuffle = yes
    CHK(player_config_skip_button_gpio() == 24);    // [player] skip_button = 24
    CHK(player_config_preview_enabled() == true);   // [player] preview = on
    CHK(player_config_preview_seconds() == 15);     // [player] preview_seconds = 15
    CHK(player_config_recursive_enabled() == true); // [player] recursive = yes

    CHK(g_present[VGM_CHIP_SN76489] == 0);          // enabled = no
    CHK(g_cs[VGM_CHIP_AY8910] == 7);                // [AY-3-8910] cs, inline comment stripped
    CHK(g_gap[VGM_CHIP_AY8910] == 55);              // GAP_US alias
    CHK(g_volume[VGM_CHIP_AY8910] == 50);           // volume key
    CHK(g_present[VGM_CHIP_SEGAPCM] == 1);          // [Sega PCM] ENABLED = TrUe
    CHK(g_cs[VGM_CHIP_SEGAPCM] == 27);              // "Pin" alias
    CHK(g_present[VGM_CHIP_YM2203] == -1);          // "maybe" rejected
    CHK(g_cs[VGM_CHIP_YM2203] == -1);               // "twelve" rejected
    CHK(g_volume[VGM_CHIP_YM2203] == -1);           // 999 out of range, rejected
    CHK(g_cs[VGM_CHIP_SCC] == 28);
    CHK(g_present[VGM_CHIP_YM2612] == -1);          // never mentioned -> untouched
    CHK(g_cs[VGM_CHIP_YM2612] == -1);
    CHK(n == 12); // sn.enabled, ay.cs, ay.gap, ay.volume, segapcm.enabled, segapcm.cs, scc.cs,
                  // player.shuffle, player.skip_button, player.preview, player.preview_seconds,
                  // player.recursive

    printf("applied=%d\n", n);
    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
