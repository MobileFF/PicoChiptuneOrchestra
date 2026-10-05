// VGM multi-MCU player -- master firmware (Raspberry Pi Pico).
//
// Mounts the SD card, plays every .vgm/.vgz file in the root directory (or,
// if vgmplay.ini's [player] root_dir is set, that folder instead) -- or, if
// [player] recursive = yes, every subdirectory under the start point too,
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
#include "flash_disk.h"
#include "spi0_bus_lock.h"
#include "cover_image.h"

#define PIN_BTN_SKIP 2 // to GND; internal pull-up enabled -- built-in default,
                        // overridable via vgmplay.ini's [player] skip_button
                        // (e.g. a clone board's "USR" button on a different
                        // GPIO than a genuine Pico's usual wiring)
static uint s_btn_skip_gpio = PIN_BTN_SKIP;

static const char *TEMP_VGM_NAME = "_vgztmp.vgm";
static const char *TEMP_VGM_PATH = "0:/_vgztmp.vgm";

// [player] flash_cache: true once flash_disk_init() has actually mounted (or
// formatted+mounted) the flash-backed "1:" volume this boot. false the whole
// session if flash_cache is off, or if it's on but init failed (logged by
// flash_disk_init() itself) -- either way play_one() just streams from the
// SD card, same as always.
static bool s_flash_cache_ready = false;

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

// OLED/TFT render-loop + core1/core0 fault health check, done here on core0
// (core1 mustn't printf -- see oled_ui.c). Normally silent unless something
// looks wrong (a HardFault was caught, the panel never answered, a push
// failed, or the loop had to re-init a wedged panel), so ordinary operation
// doesn't spam the log with a per-song line -- see core_fault.h and
// docs/design-notes.md for the HardFault this caught once (a core0 stack
// overflow corrupting core1's stack, fixed in vgz_inflate.c).
//
// `force`: print the diag line unconditionally. Needed for a tft backend
// (pass true from a spot that's reached even with no song ever playing --
// see the call sites in the retry loop below): answered()/fail/reinits are
// defined entirely in terms of an I2C ACK this panel can never give (see
// st7735_init()'s doc comment), so they're always true/0/0 for tft and the
// normal "silent unless something looks wrong" gating below would never
// fire at all -- frames is then the only live signal that core1 is actually
// running its render loop (a stuck/frozen core1 is the one failure mode
// that's still visible: frames stops incrementing between calls).
static void oled_health_check(bool force) {
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
    if (force || !oled_ui_answered() || fail > 0 || reinits > 0) {
        printf("  display: answered=%d frames=%lu shows_ok=%lu shows_fail=%lu reinits=%lu\n",
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

// [player] display = tft's once-per-folder cover image (cover_image.h):
// the alphabetically-first .jpg/.jpeg/.png found directly in a folder,
// tracked per recursion depth same as s_dir_path/s_subdir_names above --
// NOT a plain local in visit_dir(), for the same core0-stack reason (a
// DIR_PATH_BUF_SZ+256 buffer per recursion level, up to MAX_RECURSE_DEPTH
// deep, would eat a large fraction of core0's 2KB budget).
static char s_cover_name[MAX_RECURSE_DEPTH + 1][256]; // "" = none found this folder
static char s_cover_path[MAX_RECURSE_DEPTH + 1][DIR_PATH_BUF_SZ + 256];

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
// subdirectory found in that same scan. depth 0 is always visited first (the
// SD card root, or [player] root_dir if set -- main() fills in s_dir_path[0]
// before calling this); depth 0's caller is responsible for its own "nothing
// played this whole pass" messaging, using s_played_this_pass.
static void visit_dir(int depth) {
    const char *dir_path = s_dir_path[depth];
    bool recursive = player_config_recursive_enabled();

    int nfiles = 0;
    size_t names_used = 0;
    int nsubdirs = 0;
    size_t subdir_names_used = 0;
    s_cover_name[depth][0] = '\0';

    // f_findfirst()/f_findnext() each do their own separate SD transaction
    // (disk_read() call), with spi0_bus_lock.h's mutex released in the gaps
    // between them -- a folder with many entries means many such gaps for
    // core1's periodic TFT redraw to land in. Hit in the field (2026-10-02):
    // an SD card that doesn't fully let go of the shared SPI0 bus between
    // its own transactions (see sd_spi.c's sd_spi_deselect() comment) got
    // left in a bad state by this and never recovered -- every later
    // directory listing failed with FR_DISK_ERR for the rest of the session.
    //
    // First fix tried here was multicore_lockout_start/end_blocking()
    // (parking core1 entirely for the whole scan) -- it did NOT help (still
    // reproduced 2026-10-02, even on the very first "0:" listing of the
    // session). Root cause turned out to be that fix itself, used the same
    // way in flash_disk.c: multicore_lockout's pause is an asynchronous
    // inter-core interrupt that can land on core1 at ANY instruction,
    // including mid-byte inside st7735.c's spi_write_blocking() calls --
    // after spi0_bus_lock() was already taken but before spi0_bus_unlock()
    // runs. That leaves the TFT's CS asserted and spi0_bus_lock.h's mutex
    // permanently held by a now-parked core1: exactly the bad bus state
    // sd_spi.c's deselect comment warns never recovers on its own. Taking
    // the real spi0_bus_lock() here instead has no such gap -- st7735.c
    // already wraps every one of its own transactions in the SAME mutex, so
    // holding it for this whole scan just makes core1 block cooperatively at
    // its own next lock attempt (between transactions, never mid-transfer)
    // until this scan releases it. See cover_image.c's decode loop for the
    // identical reasoning and fix.
    spi0_bus_lock();
    FRESULT fr = f_findfirst(&s_scan_dir, &s_scan_info, dir_path, "*");
    if (fr != FR_OK) {
        spi0_bus_unlock();
        printf("WARNING: could not list directory %s (FatFs error %d), skipping it\n", dir_path, fr);
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
        } else if (cover_image_is_supported(s_scan_info.fname) &&
                   (s_cover_name[depth][0] == '\0' ||
                    strcasecmp(s_scan_info.fname, s_cover_name[depth]) < 0)) {
            // Alphabetically-first match wins (case-insensitive, matching
            // FAT's own case-insensitivity) -- same tie-break rule as
            // player_config_autoload()'s "vgmplay*.ini" picker.
            snprintf(s_cover_name[depth], sizeof(s_cover_name[depth]), "%s", s_scan_info.fname);
        }
        if (f_findnext(&s_scan_dir, &s_scan_info) != FR_OK) break;
    }
    f_closedir(&s_scan_dir);
    spi0_bus_unlock();

    if (nfiles > 0) {
        printf("%s: %d playable file(s)\n", dir_path, nfiles);
        s_found_this_pass += nfiles;
        // Once per folder (not per song) -- see cover_image.h.
        if (s_cover_name[depth][0]) {
            snprintf(s_cover_path[depth], sizeof(s_cover_path[depth]), "%s/%s",
                     dir_path, s_cover_name[depth]);
            cover_image_show(s_cover_path[depth]);
        } else {
            cover_image_show(NULL);
        }
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

    if (!has_extension(fname, ".vgm") && !has_extension(fname, ".vgz")) return false;

    // Whether to decompress is decided by the file's actual CONTENT (gzip
    // magic), not its extension -- some real-world VGM packs ship
    // gzip-compressed data under a plain ".vgm" name (found via
    // 調査用/Ashura-SMS/*.vgm: valid single-SN76489 songs once decompressed,
    // but silently rejected as "not a valid VGM file" when read raw because
    // the extension said not to bother). A ".vgz" that turns out to NOT be
    // gzip is handled the same way, symmetrically: read as-is.
    const char *play_path = full_path;
    if (vgz_looks_like_gzip(full_path)) {
        printf("decompressing %s ...\n", fname);
        if (!vgz_inflate_file(full_path, TEMP_VGM_PATH)) {
            printf("  ERROR: gzip decompression failed, skipping this file\n");
            return false;
        }
        play_path = TEMP_VGM_PATH;
    }

    // [player] flash_cache: copy the (already-decompressed, if it was a
    // .vgz) song into flash and play THAT copy instead, so the SD card is
    // free for the rest of this song -- see flash_disk.h. Falls back to
    // play_path unchanged (streaming from the SD card, as always) if the
    // cache isn't usable this boot, the song is too big for it, or the copy
    // itself fails partway.
    static char flash_play_path[sizeof(FLASH_DISK_CACHE_PATH)];
    if (s_flash_cache_ready &&
        flash_disk_cache_file(play_path, flash_play_path, sizeof(flash_play_path))) {
        play_path = flash_play_path;
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
        // Default 2 (play a looping song's loop section twice, then end and
        // advance to the next file); [player] loop_count overrides this
        // (skip button still cuts it short at any time either way).
        .max_loops = player_config_loop_count(),
        .poll_skip = poll_skip_button,
        .on_chips = oled_ui_set_chips, // fills in the OLED's chip list
    };
    if (!vgm_player_play(play_path, &opts)) {
        printf("  ERROR: playback aborted (bad/unsupported VGM data)\n");
    }
    oled_health_check(false); // silent unless something looks wrong -- see its own doc comment
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

    // __DATE__/__TIME__ are this translation unit's own compile time, baked
    // in at build time -- printed here (after the boot-delay wait above, not
    // before) so it's always the first thing a freshly-attached terminal
    // sees, letting "is this actually the firmware I just flashed?" be
    // answered by eye instead of guessed at (bit us 2026-10-02: a stale
    // .uf2 -- e.g. a Google Drive sync lag between this repo and the board
    // being flashed -- can silently keep old [player] behaviour alive no
    // matter how certain the SD card's vgmplay.ini looks).
    printf("build: %s %s\n", __DATE__, __TIME__);

    // [player] display's oled_ui_init() (below) shares the master's
    // physical SPI0 bus with the SD card when display = tft -- see
    // spi0_bus_lock.h. Must be ready before ANY SPI0 access at all,
    // including this very first SD mount.
    spi0_bus_lock_init();

    static FATFS fs;
    bool sd_mounted = (f_mount(&fs, "0:", 1) == FR_OK);
    if (sd_mounted) printf("SD card mounted\n");

    // Optional per-chip enable/disable + CS-pin remap + [player] settings
    // from the SD card (vgmplay.ini, or the first vgmplay*.ini). Must run
    // before slave_bus_init() -- it acts on the routing table -- and before
    // oled_ui_init() below, which needs [player] display to already be
    // known to pick a backend. Tolerates a failed mount fine (f_open just
    // fails, every setting keeps its default).
    player_config_autoload();

    oled_ui_init(sd_mounted); // I2C0/SSD1306 or SPI0/ST7735 + core1 render loop, per [player] display

    // Only mount/format the flash-backed cache volume when it's actually
    // wanted -- no point in the (one-time) formatting flash wear otherwise.
    // flash_disk_init()'s flash writes must wait for core1 (launched moments
    // ago by oled_ui_init(), above) to have registered as a multicore lockout
    // victim first -- see oled_ui_wait_for_core1_lockout_ready()'s doc
    // comment. 1s is generous; core1 does this as its very first instruction.
    if (player_config_flash_cache_enabled()) {
        if (player_config_display_is_tft())
            printf("flash cache: forced on by [player] display = tft (see player_config.c)\n");
        if (oled_ui_wait_for_core1_lockout_ready(1000)) {
            s_flash_cache_ready = flash_disk_init();
        } else {
            printf("flash cache: DISABLED -- core1 never came up\n");
        }
    }

    if (!sd_mounted) {
        // Shown regardless of [player] display now that the check runs
        // after oled_ui_init() -- previously only the OLED backend (which
        // started before the mount attempt) could ever show this.
        printf("ERROR: SD card mount failed -- check wiring and that the card is FAT32. Halting.\n");
        oled_ui_set_status("SD mount failed");
        for (;;) tight_loop_contents();
    }

    int cfg_skip_gpio = player_config_skip_button_gpio();
    if (cfg_skip_gpio >= 0) s_btn_skip_gpio = (uint)cfg_skip_gpio;
    gpio_init(s_btn_skip_gpio);
    gpio_set_dir(s_btn_skip_gpio, GPIO_IN);
    gpio_pull_up(s_btn_skip_gpio);
    // So slave_bus_init()'s reserved-pin check warns about wherever the
    // button actually is, not just its firmware default.
    slave_bus_set_skip_button_gpio(s_btn_skip_gpio);
    // Same idea for [player] display = tft's 3 GPIOs (tft_pins.h).
    slave_bus_set_display_is_tft(player_config_display_is_tft());

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

    // Each pass: walk the SD card root, or [player] root_dir if set (and, if
    // [player] recursive = yes, every subdirectory under the start point too
    // -- see visit_dir()) and play whatever is found. Re-scanning from
    // scratch every pass (rather than caching the list) means a card
    // swapped/edited between passes is picked up without a reboot, same as
    // it always was for the root-only case.
    for (;;) {
        s_played_this_pass = 0;
        s_found_this_pass = 0;
        const char *root_dir = player_config_root_dir();
        if (root_dir[0]) {
            snprintf(s_dir_path[0], DIR_PATH_BUF_SZ, "0:/%s", root_dir);
        } else {
            snprintf(s_dir_path[0], DIR_PATH_BUF_SZ, "0:");
        }
        visit_dir(0);

        if (s_found_this_pass == 0) {
            if (root_dir[0]) {
                printf("no .vgm/.vgz files found under \"%s\"%s, retrying...\n", root_dir,
                       player_config_recursive_enabled() ? " (or any subfolder of it)" : "");
            } else {
                printf("%s", player_config_recursive_enabled()
                           ? "no .vgm/.vgz files found on the SD card (root or any subfolder), retrying...\n"
                           : "no .vgm/.vgz files found on the SD card root, retrying...\n");
            }
            oled_ui_set_status("No .vgm files on card");
            oled_health_check(true); // force=true -- no song ever reaches play_one()'s
                                     // own call to this while stuck here; see this
                                     // function's own doc comment for why tft needs force.
            sleep_ms(1000); // avoid a tight spin on an empty card
        } else if (s_played_this_pass == 0) {
            // Every file this pass was skipped (all need chips that are
            // disabled/absent in vgmplay.ini). Don't re-scan the card at full
            // tilt -- wait a beat so the skip lines stay readable.
            printf("no files playable with the current vgmplay.ini chip config, retrying...\n");
            oled_ui_set_status("No playable songs");
            oled_health_check(true); // force=true -- see comment above
            sleep_ms(3000);
        }
    }
}
