// VGM multi-MCU player -- master firmware (Raspberry Pi Pico).
//
// Mounts the SD card, plays every .vgm/.vgz file in the root directory in
// case-insensitive sorted order (looping the whole list forever),
// dispatching register writes to the slave boards over slave_bus. See
// docs/circuit.md for wiring and docs/design-notes.md for VGM command
// coverage.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "ff.h"

#include "slave_bus.h"
#include "vgm_chips.h"
#include "vgm_player.h"
#include "vgz_inflate.h"
#include "oled_ui.h"
#include "player_config.h"

#define PIN_BTN_SKIP 2 // to GND; internal pull-up enabled

static const char *TEMP_VGM_NAME = "_vgztmp.vgm";
static const char *TEMP_VGM_PATH = "0:/_vgztmp.vgm";

static bool has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + (nlen - elen), ext) == 0;
}

static bool poll_skip_button(void) {
    return !gpio_get(PIN_BTN_SKIP);
}

// Playable filenames for one pass are collected into s_names (NUL-terminated,
// packed back to back) with s_name_off[] indexing each one, so sorting only
// shuffles 2-byte offsets instead of copying ~256-byte FILINFO names around.
#define MAX_FILES 256
#define NAMES_BUF_SZ 12288
static char s_names[NAMES_BUF_SZ];
static uint16_t s_name_off[MAX_FILES];

static int name_cmp(const void *a, const void *b) {
    return strcasecmp(s_names + *(const uint16_t *)a, s_names + *(const uint16_t *)b);
}

static bool is_playable(const FILINFO *info) {
    if (info->fattrib & AM_DIR) return false;
    if (strcasecmp(info->fname, TEMP_VGM_NAME) == 0) return false;
    return has_extension(info->fname, ".vgm") || has_extension(info->fname, ".vgz");
}

// Returns true if playback was attempted, false if the file was skipped
// (not a .vgm/.vgz, decompression failed, or it needs a chip that vgmplay.ini
// has disabled -- see the chip-availability check below).
static bool play_one(const char *dir_path, const char *fname) {
    char full_path[300];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, fname);

    const char *play_path = full_path;
    if (has_extension(fname, ".vgz")) {
        printf("decompressing %s ...\n", fname);
        if (!vgz_inflate_file(full_path, TEMP_VGM_PATH)) {
            printf("  ERROR: gzip decompression failed, skipping this file\n");
            return false;
        }
        play_path = TEMP_VGM_PATH;
    } else if (!has_extension(fname, ".vgm")) {
        return false;
    }

    // Skip a song that needs a chip this build doesn't have a slave wired up
    // for -- either no slave exists for it, or vgmplay.ini set it
    // `enabled = no`. vgm_player would just drop that chip's register writes,
    // leaving a voice (often the melody, sometimes the whole song) silent, so
    // move on to the next file instead.
    uint32_t used = 0;
    if (vgm_player_scan_chips(play_path, &used)) {
        uint32_t missing = 0;
        for (int c = 0; c < VGM_CHIP_COUNT; c++)
            if ((used & (1u << c)) && !slave_bus_has_chip((vgm_chip_id_t)c))
                missing |= 1u << c;
        if (missing) {
            printf("skipping %s -- needs disabled/absent chip(s):", fname);
            for (int c = 0; c < VGM_CHIP_COUNT; c++)
                if (missing & (1u << c)) printf(" %s", vgm_chip_name((vgm_chip_id_t)c));
            printf("\n");
            return false;
        }
    }

    printf("playing: %s\n", fname);
    oled_ui_set_song(fname);
    vgm_player_opts_t opts = {
        .loop_enabled = true,
        .max_loops = 2, // play a looping song's loop section twice, then end
                        // and advance to the next file (skip button still
                        // cuts it short at any time)
        .poll_skip = poll_skip_button,
        .on_chips = oled_ui_set_chips, // fills in the OLED's chip list
    };
    if (!vgm_player_play(play_path, &opts)) {
        printf("  ERROR: playback aborted (bad/unsupported VGM data)\n");
    }
    sleep_ms(2000); // pause between songs so the next one doesn't start instantly
    return true;
}

int main(void) {
    stdio_init_all();
    // See docs/design-notes.md "ログの確認方法": this goes out both GPIO0
    // (UART, 115200 baud) and the USB cable used to flash/power the board.
    printf("\n=== VGM multi-MCU player (master) starting ===\n");

    // Debug aid: hold off startup so there is time to plug in USB and open a
    // terminal before the first log lines fly past. Counts down once per
    // second so a mid-window connection still sees it. Defaults to 10 s in a
    // VGM_MASTER_DEBUG_TRACE build, 0 (off) otherwise; override with
    // -DVGM_MASTER_BOOT_DELAY_S=N.
#ifndef VGM_MASTER_BOOT_DELAY_S
#if VGM_MASTER_DEBUG_TRACE
#define VGM_MASTER_BOOT_DELAY_S 10
#else
#define VGM_MASTER_BOOT_DELAY_S 0
#endif
#endif
    for (int s = VGM_MASTER_BOOT_DELAY_S; s > 0; s--) {
        printf("  starting in %d s ...\n", s);
        sleep_ms(1000);
    }

    gpio_init(PIN_BTN_SKIP);
    gpio_set_dir(PIN_BTN_SKIP, GPIO_IN);
    gpio_pull_up(PIN_BTN_SKIP);

    oled_ui_init(); // I2C0 on GPIO0/1 + core1 render loop (no-op if no panel)

    static FATFS fs;
    if (f_mount(&fs, "0:", 1) != FR_OK) {
        printf("ERROR: SD card mount failed -- check wiring and that the card is FAT32. Halting.\n");
        oled_ui_set_status("SD mount failed");
        for (;;) tight_loop_contents();
    }
    printf("SD card mounted\n");

    // Optional per-chip enable/disable + CS-pin remap from the SD card
    // (vgmplay.ini, or the first vgmplay*.ini). Must run before
    // slave_bus_init() -- it acts on the routing table.
    player_config_autoload();

    slave_bus_init();
    printf("slave bus initialized\n");

    const char *dir_path = "0:";
    for (;;) {
        DIR dir;
        FILINFO info;
        if (f_findfirst(&dir, &info, dir_path, "*") != FR_OK) {
            printf("ERROR: could not list SD card root directory, retrying...\n");
            sleep_ms(1000);
            continue;
        }

        // Pass 1: collect every playable filename into s_names[].
        int nfiles = 0;
        size_t names_used = 0;
        while (info.fname[0] != 0) {
            if (is_playable(&info)) {
                size_t len = strlen(info.fname) + 1;
                if (nfiles >= MAX_FILES || names_used + len > sizeof(s_names)) {
                    printf("WARNING: too many playable files for the buffer -- "
                           "playing only the first %d this pass\n", nfiles);
                    break;
                }
                memcpy(s_names + names_used, info.fname, len);
                s_name_off[nfiles++] = (uint16_t)names_used;
                names_used += len;
            }
            if (f_findnext(&dir, &info) != FR_OK) break;
        }
        f_closedir(&dir);

        if (nfiles == 0) {
            printf("no .vgm/.vgz files found on the SD card root, retrying...\n");
            oled_ui_set_status("No .vgm files on card");
            sleep_ms(1000); // avoid a tight spin on an empty card
            continue;
        }

        // Pass 2: play them in case-insensitive sorted order.
        qsort(s_name_off, nfiles, sizeof(s_name_off[0]), name_cmp);
        int played = 0;
        for (int i = 0; i < nfiles; i++) {
            if (play_one(dir_path, s_names + s_name_off[i])) played++;
        }
        if (played == 0) {
            // Every file this pass was skipped (all need chips that are
            // disabled/absent in vgmplay.ini). Don't re-scan the card at full
            // tilt -- wait a beat so the skip lines stay readable.
            printf("no files playable with the current vgmplay.ini chip config, retrying...\n");
            oled_ui_set_status("No playable songs");
            sleep_ms(3000);
        }
    }
}
