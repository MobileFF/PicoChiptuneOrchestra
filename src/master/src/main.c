// VGM multi-MCU player -- master firmware (Raspberry Pi Pico).
//
// Mounts the SD card, plays every .vgm/.vgz file in the root directory --
// or, if vgmplay.ini's [player] recursive = yes, every subdirectory too,
// one folder at a time (see visit_dir()) -- in case-insensitive sorted order
// (looping forever) or, if [player] shuffle = yes, a freshly-randomised
// order per folder each pass, dispatching register writes to the slave
// boards over slave_bus. See docs/circuit.md for wiring and
// docs/design-notes.md for VGM command coverage.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "ff.h"

#include "slave_bus.h"
#include "vgm_chips.h"
#include "vgm_player.h"
#include "vgz_inflate.h"
#include "oled_ui.h"
#include "core_fault.h"
#include "player_config.h"

#define PIN_BTN_SKIP 2 // to GND; internal pull-up enabled -- built-in default,
                        // overridable via vgmplay.ini's [player] skip_button
                        // (e.g. a clone board's "USR" button on a different
                        // GPIO than a genuine Pico's usual wiring)
static uint s_btn_skip_gpio = PIN_BTN_SKIP;

static const char *TEMP_VGM_NAME = "_vgztmp.vgm";
static const char *TEMP_VGM_PATH = "0:/_vgztmp.vgm";

static bool has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + (nlen - elen), ext) == 0;
}

// Also doubles as the [player] preview cutoff: when enabled, treat "song
// time so far reached the preview length" the same as a real button press,
// so vgm_player_play() ends the song and main.c moves on to the next file --
// no separate code path in vgm_player.c needed.
static bool poll_skip_button(void) {
    if (!gpio_get(s_btn_skip_gpio)) return true;
    if (player_config_preview_enabled() &&
        vgm_player_elapsed_seconds() >= player_config_preview_seconds())
        return true;
    return false;
}

// OLED render-loop + core1/core0 fault health check, done here on core0
// (core1 mustn't printf -- see oled_ui.c). Silent unless something looks
// wrong (a HardFault was caught, the panel never answered, a push failed, or
// the loop had to re-init a wedged panel), so normal operation doesn't spam
// the log with a per-song line -- see core_fault.h and docs/design-notes.md
// for the HardFault this caught once (a core0 stack overflow corrupting
// core1's stack, fixed in vgz_inflate.c).
static void oled_health_check(void) {
    if (g_core_fault.count) {
        printf("  *** core%u FAULT vect=%u count=%lu pc=%08lx lr=%08lx psr=%08lx\n",
               g_core_fault.core, g_core_fault.vect, (unsigned long)g_core_fault.count,
               (unsigned long)g_core_fault.pc, (unsigned long)g_core_fault.lr,
               (unsigned long)g_core_fault.psr);
        printf("      r0=%08lx r1=%08lx r2=%08lx r3=%08lx r12=%08lx\n",
               (unsigned long)g_core_fault.r0, (unsigned long)g_core_fault.r1,
               (unsigned long)g_core_fault.r2, (unsigned long)g_core_fault.r3,
               (unsigned long)g_core_fault.r12);
    }
    uint32_t frames = 0, ok = 0, fail = 0, reinits = 0;
    oled_ui_diag(&frames, &ok, &fail, &reinits);
    if (!oled_ui_answered() || fail > 0 || reinits > 0) {
        printf("  OLED: answered=%d frames=%lu shows_ok=%lu shows_fail=%lu reinits=%lu\n",
               (int)oled_ui_answered(), (unsigned long)frames, (unsigned long)ok,
               (unsigned long)fail, (unsigned long)reinits);
    }
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

// Fisher-Yates, in place. get_rand_32() (pico_rand -- see CMakeLists.txt) is
// a hardware-seeded PRNG; playlist shuffling has no need for cryptographic
// quality, just a fresh, non-repeating-pattern order each pass (see
// player_config.h's [player] shuffle key). `% (i + 1)` has a slight modulo
// bias for large i, irrelevant at MAX_FILES=256.
static void shuffle_name_off(uint16_t *off, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(get_rand_32() % (uint32_t)(i + 1));
        uint16_t tmp = off[i]; off[i] = off[j]; off[j] = tmp;
    }
}

static bool is_playable(const FILINFO *info) {
    if (info->fattrib & AM_DIR) return false;
    if (strcasecmp(info->fname, TEMP_VGM_NAME) == 0) return false;
    return has_extension(info->fname, ".vgm") || has_extension(info->fname, ".vgz");
}

// --- [player] recursive: walk every subdirectory, playing each folder's ----
// --- files in turn, instead of just the SD card root -----------------------
//
// core0's stack is a mere 2KB (see docs/design-notes.md's HardFault writeup --
// a single oversized stack-local buffer once overran it and silently trashed
// core1's adjacent stack), so this is deliberately NOT plain recursion with
// per-call locals: every buffer big enough to matter (the directory path, the
// list of subdirectory names found at that level) is `static` and indexed by
// `depth`, and the FatFs DIR/FILINFO scan handles are a SINGLE shared static
// pair, reused level to level -- safe because each level's own scan always
// finishes (and its DIR is closed) before that level recurses into any child,
// so a child's reuse of the shared scan state can never clobber a parent that
// still needs it. visit_dir()'s own stack frame is then just a handful of
// scalars regardless of how deep the recursion goes.
//
// MAX_RECURSE_DEPTH bounds both the static RAM this uses and the worst-case
// stack depth; 4 comfortably covers any realistic "console/game/album"-style
// folder layout. DIR_PATH_BUF_SZ is sized for that depth's worth of ordinary
// folder names, not the pathological case of every single one being near
// FatFs's 255-char LFN limit -- play_one()'s own path buffer adds separate
// headroom for a maximally-long leaf filename, so only folder *names* need
// to fit here.
#define MAX_RECURSE_DEPTH 4
#define DIR_PATH_BUF_SZ 200
#define MAX_SUBDIRS 64
#define SUBDIR_NAMES_BUF_SZ 2048

static char s_dir_path[MAX_RECURSE_DEPTH + 1][DIR_PATH_BUF_SZ];
static char s_subdir_names[MAX_RECURSE_DEPTH + 1][SUBDIR_NAMES_BUF_SZ];
static uint16_t s_subdir_off[MAX_RECURSE_DEPTH + 1][MAX_SUBDIRS];
static DIR s_scan_dir;
static FILINFO s_scan_info;
static int s_played_this_pass;
static int s_found_this_pass; // total playable files seen across every folder visited this pass

// A subdirectory worth descending into for [player] recursive -- real
// directories only, and not one of the OS-junk folders (e.g. Windows'
// "System Volume Information") that tend to appear on an SD card that's ever
// been plugged into a PC.
static bool is_real_subdir(const FILINFO *info) {
    if (!(info->fattrib & AM_DIR)) return false;
    if (info->fattrib & (AM_HID | AM_SYS)) return false;
    return true;
}

static bool play_one(const char *dir_path, const char *fname); // fwd decl

// Scans `s_dir_path[depth]` once, playing every .vgm/.vgz it finds there
// (sorted or shuffled exactly like the non-recursive root-only path always
// did), then -- only when [player] recursive is on -- recurses into every
// subdirectory found in that same scan. depth 0 is always visited (the SD
// card root); depth 0's caller is responsible for its own "nothing played
// this whole pass" messaging, using s_played_this_pass.
static void visit_dir(int depth) {
    const char *dir_path = s_dir_path[depth];
    bool recursive = player_config_recursive_enabled();

    int nfiles = 0;
    size_t names_used = 0;
    int nsubdirs = 0;
    size_t subdir_names_used = 0;

    if (f_findfirst(&s_scan_dir, &s_scan_info, dir_path, "*") != FR_OK) {
        printf("WARNING: could not list directory %s, skipping it\n", dir_path);
        return;
    }
    while (s_scan_info.fname[0] != 0) {
        if (recursive && depth < MAX_RECURSE_DEPTH && is_real_subdir(&s_scan_info)) {
            size_t len = strlen(s_scan_info.fname) + 1;
            if (nsubdirs >= MAX_SUBDIRS || subdir_names_used + len > SUBDIR_NAMES_BUF_SZ) {
                printf("WARNING: too many subdirectories in %s -- only visiting the first %d\n",
                       dir_path, nsubdirs);
            } else {
                memcpy(s_subdir_names[depth] + subdir_names_used, s_scan_info.fname, len);
                s_subdir_off[depth][nsubdirs++] = (uint16_t)subdir_names_used;
                subdir_names_used += len;
            }
        } else if (is_playable(&s_scan_info)) {
            size_t len = strlen(s_scan_info.fname) + 1;
            if (nfiles >= MAX_FILES || names_used + len > sizeof(s_names)) {
                printf("WARNING: too many playable files in %s -- playing only the first %d\n",
                       dir_path, nfiles);
            } else {
                memcpy(s_names + names_used, s_scan_info.fname, len);
                s_name_off[nfiles++] = (uint16_t)names_used;
                names_used += len;
            }
        }
        if (f_findnext(&s_scan_dir, &s_scan_info) != FR_OK) break;
    }
    f_closedir(&s_scan_dir);

    if (nfiles > 0) {
        printf("%s: %d playable file(s)\n", dir_path, nfiles);
        s_found_this_pass += nfiles;
        if (player_config_shuffle_enabled()) {
            shuffle_name_off(s_name_off, nfiles);
        } else {
            qsort(s_name_off, nfiles, sizeof(s_name_off[0]), name_cmp);
        }
        for (int i = 0; i < nfiles; i++) {
            if (play_one(dir_path, s_names + s_name_off[i])) s_played_this_pass++;
        }
    }

    // Recurse only after this level is fully done with the shared scan state
    // and with s_names/s_name_off -- see this function's own doc comment.
    for (int i = 0; i < nsubdirs; i++) {
        const char *sub = s_subdir_names[depth] + s_subdir_off[depth][i];
        snprintf(s_dir_path[depth + 1], DIR_PATH_BUF_SZ, "%s/%s", dir_path, sub);
        visit_dir(depth + 1);
    }
}

// Returns true if playback was attempted, false if the file was skipped
// (not a .vgm/.vgz, decompression failed, or it needs a chip that vgmplay.ini
// has disabled -- see the chip-availability check below).
static bool play_one(const char *dir_path, const char *fname) {
    // dir_path can be several [player] recursive levels deep (see
    // DIR_PATH_BUF_SZ above) plus a long filename, so this needs a bit more
    // headroom than a root-only "0:/name.vgm" ever did. A combination that's
    // both maximally deep AND has a maximally long name at every level would
    // still truncate here -- snprintf() makes that a graceful "file not
    // found" (logged, skipped) rather than a buffer overrun, and it's not a
    // realistic real-world folder layout.
    char full_path[DIR_PATH_BUF_SZ + 256];
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
    oled_health_check();
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
    // slave_bus_init() -- it acts on the routing table -- and before the
    // skip-button gpio_init below, which needs to know the final pin.
    player_config_autoload();

    int cfg_skip_gpio = player_config_skip_button_gpio();
    if (cfg_skip_gpio >= 0) s_btn_skip_gpio = (uint)cfg_skip_gpio;
    gpio_init(s_btn_skip_gpio);
    gpio_set_dir(s_btn_skip_gpio, GPIO_IN);
    gpio_pull_up(s_btn_skip_gpio);
    // So slave_bus_init()'s reserved-pin check warns about wherever the
    // button actually is, not just its firmware default.
    slave_bus_set_skip_button_gpio(s_btn_skip_gpio);

    slave_bus_init();
    printf("slave bus initialized\n");

    // Slave settle window. Each slave board powers on independently and only
    // its own clock/vreg settle + core1 launch + SPI-RX sync gates when it
    // starts hearing the bus. The master can reach here (SD mount + playlist
    // scan) faster than a slave finishes booting -- and the FIRST song's
    // one-shot setup, notably the YM2612 PCM bank upload (never re-sent),
    // then goes out to a slave that isn't listening yet, leaving that chip's
    // PCM silent for the whole first song. Non-deterministic across power-ups
    // because the two boot times race. Hold here, re-broadcasting RESET as
    // clean sync points, so any slave that boots within the window is synced
    // and reset before playback. (preset 0 here; the real per-song preset is
    // sent by vgm_player_play and re-asserted once a second during playback.)
    for (int i = 0; i < 8; i++) {
        for (int c = 0; c < VGM_CHIP_COUNT; c++)
            if (slave_bus_has_chip((vgm_chip_id_t)c))
                slave_bus_reset((vgm_chip_id_t)c, 0);
        sleep_ms(250);
    }
    printf("slave settle window done\n");

    // Each pass: walk the SD card root (and, if [player] recursive = yes,
    // every subdirectory under it too -- see visit_dir()) and play whatever
    // is found. Re-scanning from scratch every pass (rather than caching the
    // list) means a card swapped/edited between passes is picked up without
    // a reboot, same as it always was for the root-only case.
    for (;;) {
        s_played_this_pass = 0;
        s_found_this_pass = 0;
        snprintf(s_dir_path[0], DIR_PATH_BUF_SZ, "0:");
        visit_dir(0);

        if (s_found_this_pass == 0) {
            printf("%s", player_config_recursive_enabled()
                       ? "no .vgm/.vgz files found on the SD card (root or any subfolder), retrying...\n"
                       : "no .vgm/.vgz files found on the SD card root, retrying...\n");
            oled_ui_set_status("No .vgm files on card");
            sleep_ms(1000); // avoid a tight spin on an empty card
        } else if (s_played_this_pass == 0) {
            // Every file this pass was skipped (all need chips that are
            // disabled/absent in vgmplay.ini). Don't re-scan the card at full
            // tilt -- wait a beat so the skip lines stay readable.
            printf("no files playable with the current vgmplay.ini chip config, retrying...\n");
            oled_ui_set_status("No playable songs");
            sleep_ms(3000);
        }
    }
}
