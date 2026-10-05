#include "player_config.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ff.h"
#include "slave_bus.h"
#include "vgm_chips.h"

// --- section-name -> chip id --------------------------------------------------

// Keys are lowercase and stripped of '-', '_' and spaces; a raw section name
// is normalised the same way before comparison, so "[AY-3-8910]",
// "[ay_8910]" and "[Sega PCM]" all match.
static bool name_matches(const char *raw, const char *key) {
    while (*raw) {
        char c = (char)tolower((unsigned char)*raw++);
        if (c == '-' || c == '_' || c == ' ') continue;
        if (c != *key++) return false;
    }
    return *key == '\0';
}

// Not a chip -- see player_config.h's [player] section doc comment.
#define SECTION_PLAYER (-2)
static bool s_shuffle_enabled = false;
static int s_skip_button_gpio = -1; // -1 = not set, caller keeps its own default
static bool s_preview_enabled = false;
static uint32_t s_preview_seconds = 30;
static bool s_recursive_enabled = false;
#define ROOT_DIR_BUF_SZ 128
static char s_root_dir[ROOT_DIR_BUF_SZ] = ""; // "" = SD card root
static uint8_t s_loop_count = 2; // matches main.c's previous hardcoded max_loops
static bool s_flash_cache_enabled = false; // opt-in: see flash_disk.h
static bool s_display_is_tft = false; // false = oled (default), true = tft

bool player_config_shuffle_enabled(void) { return s_shuffle_enabled; }
int player_config_skip_button_gpio(void) { return s_skip_button_gpio; }
bool player_config_preview_enabled(void) { return s_preview_enabled; }
uint32_t player_config_preview_seconds(void) { return s_preview_seconds; }
bool player_config_recursive_enabled(void) { return s_recursive_enabled; }
uint8_t player_config_loop_count(void) { return s_loop_count; }
const char *player_config_root_dir(void) { return s_root_dir; }
// display = tft USED TO force this on regardless of the ini's own
// flash_cache setting, on the theory that TFT streaming VGM straight off
// the SD card for a whole song meant continuous SD reads contending with
// core1's periodic TFT redraw for the whole song. That theory predated
// finding the actual root cause of this project's long FR_DISK_ERR saga --
// SPI1 (slave chip bus) electrical crosstalk onto SPI0 (the SD card) from
// physically adjacent GPIOs, fixed in slave_bus.c via
// gpio_set_slew_rate()/gpio_set_drive_strength() -- which had nothing to do
// with TFT vs. OLED or with flash_cache at all. The forcing was removed
// 2026-10-06 and display=tft + flash_cache=no confirmed stable on real
// hardware (multiple songs, no FR_DISK_ERR, TFT rendering normally) -- see
// docs/design-notes.md's TFT writeup. flash_cache itself stays as an
// opt-in debug/special-case knob (e.g. freeing the SD card's SPI bus
// entirely for something else); it's just no longer coupled to display.
bool player_config_flash_cache_enabled(void) { return s_flash_cache_enabled; }
bool player_config_display_is_tft(void) { return s_display_is_tft; }

static int lookup_chip(const char *raw) {
    static const struct { const char *key; int id; } KEYS[] = {
        {"sn76489", VGM_CHIP_SN76489},
        {"ym2413",  VGM_CHIP_YM2413},
        {"ym2612",  VGM_CHIP_YM2612},
        {"ay8910",  VGM_CHIP_AY8910},
        {"ay38910", VGM_CHIP_AY8910}, // "ay-3-8910"
        {"ym2151",  VGM_CHIP_YM2151},
        {"ym2203",  VGM_CHIP_YM2203},
        {"scc",     VGM_CHIP_SCC},
        {"k051649", VGM_CHIP_SCC},
        {"segapcm", VGM_CHIP_SEGAPCM},
        {"sn764892", VGM_CHIP_SN76489_2}, // "[sn76489_2]" / "[sn76489-2]" / "[sn76489 2]"
    };
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
        if (name_matches(raw, KEYS[i].key)) return KEYS[i].id;
    return -1;
}

// --- tiny value parsers ----------------------------------------------------

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static bool parse_bool(const char *v, bool *out) {
    static const char *T[] = {"1", "on", "yes", "true", "enabled", "enable"};
    static const char *F[] = {"0", "off", "no", "false", "disabled", "disable"};
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcasecmp(v, T[i])) { *out = true; return true; }
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++)
        if (!strcasecmp(v, F[i])) { *out = false; return true; }
    return false;
}

static bool parse_uint(const char *v, uint32_t *out) {
    if (!*v) return false;
    uint32_t n = 0;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10u + (uint32_t)(*p - '0');
    }
    *out = n;
    return true;
}

// --- parser --------------------------------------------------------------

int player_config_apply(const char *text) {
    char line[128];
    int applied = 0;
    int cur = -1; // current section: [chip] id, SECTION_PLAYER, or -1 = none/unknown
    const char *p = text;

    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
        line[n] = '\0';
        while (*p && *p != '\n') p++; // discard any overflow past the line buffer
        if (*p == '\n') p++;

        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) { printf("config: malformed section line: %s\n", s); continue; }
            *close = '\0';
            char *name = trim(s + 1);
            if (name_matches(name, "player")) {
                cur = SECTION_PLAYER;
            } else {
                cur = lookup_chip(name);
                if (cur < 0) printf("config: unknown chip section [%s], skipping its keys\n", name);
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { printf("config: ignoring line without '=': %s\n", s); continue; }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        for (char *c = val; *c; c++)
            if (*c == '#' || *c == ';') { *c = '\0'; break; } // strip inline comment
        val = trim(val);

        if (cur == SECTION_PLAYER) {
            if (!strcasecmp(key, "shuffle")) {
                bool b;
                if (parse_bool(val, &b)) { s_shuffle_enabled = b; applied++; }
                else printf("config: bad boolean '%s' for %s\n", val, key);
            } else if (!strcasecmp(key, "skip_button") || !strcasecmp(key, "skip_gpio") ||
                       !strcasecmp(key, "skip_pin")) {
                uint32_t u;
                if (parse_uint(val, &u) && u <= 28) { s_skip_button_gpio = (int)u; applied++; }
                else printf("config: bad number '%s' for %s (0-28)\n", val, key);
            } else if (!strcasecmp(key, "preview")) {
                bool b;
                if (parse_bool(val, &b)) { s_preview_enabled = b; applied++; }
                else printf("config: bad boolean '%s' for %s\n", val, key);
            } else if (!strcasecmp(key, "preview_seconds")) {
                uint32_t u;
                if (parse_uint(val, &u) && u > 0) { s_preview_seconds = u; applied++; }
                else printf("config: bad number '%s' for %s (>0)\n", val, key);
            } else if (!strcasecmp(key, "recursive")) {
                bool b;
                if (parse_bool(val, &b)) { s_recursive_enabled = b; applied++; }
                else printf("config: bad boolean '%s' for %s\n", val, key);
            } else if (!strcasecmp(key, "root_dir") || !strcasecmp(key, "rootdir") ||
                       !strcasecmp(key, "folder") || !strcasecmp(key, "dir")) {
                // Strip an optional FatFs-style "0:" drive prefix and any
                // leading/trailing slashes, so "0:/GAMES/Sega/", "/GAMES/Sega"
                // and "GAMES/Sega" all end up stored the same way.
                const char *v = val;
                if (!strncasecmp(v, "0:", 2)) v += 2;
                while (*v == '/') v++;
                size_t vlen = strlen(v);
                while (vlen > 0 && v[vlen - 1] == '/') vlen--;
                if (vlen >= sizeof(s_root_dir)) {
                    printf("config: root_dir '%s' too long (max %u chars), ignored\n",
                           val, (unsigned)sizeof(s_root_dir) - 1);
                } else {
                    memcpy(s_root_dir, v, vlen);
                    s_root_dir[vlen] = '\0';
                    applied++;
                }
            } else if (!strcasecmp(key, "loop_count") || !strcasecmp(key, "loopcount") ||
                       !strcasecmp(key, "loops") || !strcasecmp(key, "max_loops")) {
                uint32_t u;
                if (parse_uint(val, &u) && u <= 255) { s_loop_count = (uint8_t)u; applied++; }
                else printf("config: bad number '%s' for %s (0-255)\n", val, key);
            } else if (!strcasecmp(key, "flash_cache") || !strcasecmp(key, "flashcache") ||
                       !strcasecmp(key, "cache")) {
                bool b;
                if (parse_bool(val, &b)) { s_flash_cache_enabled = b; applied++; }
                else printf("config: bad boolean '%s' for %s\n", val, key);
            } else if (!strcasecmp(key, "display")) {
                if (!strcasecmp(val, "oled")) { s_display_is_tft = false; applied++; }
                else if (!strcasecmp(val, "tft")) { s_display_is_tft = true; applied++; }
                else printf("config: bad value '%s' for display (oled|tft)\n", val);
            } else {
                printf("config: unknown key '%s' in [player], ignored\n", key);
            }
            continue;
        }

        if (cur < 0) {
            printf("config: '%s' is outside any [chip] section, skipped\n", key);
            continue;
        }

        if (!strcasecmp(key, "enabled") || !strcasecmp(key, "enable") ||
            !strcasecmp(key, "present") || !strcasecmp(key, "on")) {
            bool b;
            if (parse_bool(val, &b)) { slave_bus_set_present((vgm_chip_id_t)cur, b); applied++; }
            else printf("config: bad boolean '%s' for %s\n", val, key);
        } else if (!strcasecmp(key, "cs") || !strcasecmp(key, "cs_gpio") ||
                   !strcasecmp(key, "cs_pin") || !strcasecmp(key, "pin")) {
            uint32_t u;
            if (parse_uint(val, &u)) { slave_bus_set_cs_gpio((vgm_chip_id_t)cur, (unsigned)u); applied++; }
            else printf("config: bad number '%s' for %s\n", val, key);
        } else if (!strcasecmp(key, "gap") || !strcasecmp(key, "gap_us")) {
            uint32_t u;
            if (parse_uint(val, &u)) { slave_bus_set_gap_us((vgm_chip_id_t)cur, u); applied++; }
            else printf("config: bad number '%s' for %s\n", val, key);
        } else if (!strcasecmp(key, "volume") || !strcasecmp(key, "vol")) {
            uint32_t u;
            if (parse_uint(val, &u) && u <= 255) { slave_bus_set_volume_pct((vgm_chip_id_t)cur, (uint8_t)u); applied++; }
            else printf("config: bad number '%s' for %s (0-255)\n", val, key);
        } else {
            printf("config: unknown key '%s', ignored\n", key);
        }
    }
    return applied;
}

// core0's stack is a mere 2KB (see docs/design-notes.md's HardFault
// writeup/main.c's visit_dir() comment for the established pattern this
// follows). FIL/DIR/FILINFO are all much bigger than they look -- FatFs is
// built with FF_FS_TINY=0 (FIL embeds a 512-byte FF_MAX_SS sector buffer)
// and FF_USE_LFN!=0 (FILINFO embeds a 256-byte long-filename buffer) -- so
// these, plus `pick`/`path` below, were previously plain locals of
// player_config_load()/player_config_autoload()/player_config_apply(),
// three functions that call into each other (main() -> autoload() ->
// load() -> apply()), stacking all of their frames at once. Measured with
// `-fstack-usage` (2026-10-03): autoload() alone was 1528 bytes, load() 648,
// apply() 184 -- 2360 bytes of nested frames before counting main()'s own or
// any call/return overhead, comfortably over the 2KB budget. The overflow
// silently clobbered whatever static variable happened to sit just past
// core0's stack region -- in the field this landed on s_display_is_tft,
// making vgmplay.ini look like it contained "display = tft" (and therefore
// forcing flash_cache on too) on every single boot even with no such key
// anywhere in the file, which in turn made every directory listing fail
// with FR_DISK_ERR from the TFT/SD SPI0-sharing bugs those settings enable
// -- despite [player] display never actually being set. None of this was a
// SPI0/display bug at all; every fix attempted at that layer necessarily
// did nothing. `static` here (never reentrant or concurrent: this whole
// call chain runs once, on core0, before core1 is even launched) moves
// these out of the stack entirely, the same fix already applied to
// main.c's visit_dir() for the identical reason.
static FIL s_cfg_file;
static DIR s_cfg_dir;
static FILINFO s_cfg_info;
static char s_cfg_pick[256]; // matches FILINFO.fname when long filenames are enabled
static char s_cfg_path[4 + sizeof(s_cfg_pick)];

int player_config_load(const char *path) {
    if (f_open(&s_cfg_file, path, FA_READ) != FR_OK) {
        printf("config: no %s on card, using built-in defaults\n", path);
        return 0;
    }
    static char buf[4096];
    UINT br = 0;
    FRESULT fr = f_read(&s_cfg_file, buf, sizeof(buf) - 1, &br);
    f_close(&s_cfg_file);
    if (fr != FR_OK) {
        printf("config: read error on %s, using built-in defaults\n", path);
        return 0;
    }
    buf[br] = '\0';
    if (br == sizeof(buf) - 1)
        printf("config: %s exceeds %u bytes; only the first part is parsed\n",
               path, (unsigned)(sizeof(buf) - 1));

    const char *start = buf;
    if (br >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF)
        start = buf + 3; // skip a UTF-8 BOM (Windows editors)

    int n = player_config_apply(start);
    printf("config: %d setting(s) applied from %s\n", n, path);
    return n;
}

static bool ci_ends_with(const char *name, const char *suffix) {
    size_t nl = strlen(name), sl = strlen(suffix);
    return nl >= sl && strcasecmp(name + nl - sl, suffix) == 0;
}

int player_config_autoload(void) {
    // Preferred exact name.
    if (f_open(&s_cfg_file, "0:/vgmplay.ini", FA_READ) == FR_OK) {
        f_close(&s_cfg_file);
        return player_config_load("0:/vgmplay.ini");
    }

    // Otherwise the alphabetically-first "vgmplay*.ini" in the root, so a
    // card can carry e.g. "vgmplay_scc.ini" and it's obvious at a glance
    // which chip set that card is wired for. Sorted (not raw dir order) so
    // the choice is deterministic when several are present.
    s_cfg_pick[0] = '\0';
    if (f_findfirst(&s_cfg_dir, &s_cfg_info, "0:", "*") == FR_OK) {
        while (s_cfg_info.fname[0]) {
            if (!(s_cfg_info.fattrib & AM_DIR) &&
                strncasecmp(s_cfg_info.fname, "vgmplay", 7) == 0 &&
                ci_ends_with(s_cfg_info.fname, ".ini") &&
                (s_cfg_pick[0] == '\0' || strcasecmp(s_cfg_info.fname, s_cfg_pick) < 0)) {
                snprintf(s_cfg_pick, sizeof(s_cfg_pick), "%s", s_cfg_info.fname);
            }
            if (f_findnext(&s_cfg_dir, &s_cfg_info) != FR_OK) break;
        }
        f_closedir(&s_cfg_dir);
    }

    if (s_cfg_pick[0] == '\0') {
        printf("config: no vgmplay*.ini on card, using built-in defaults\n");
        return 0;
    }
    snprintf(s_cfg_path, sizeof(s_cfg_path), "0:/%s", s_cfg_pick);
    printf("config: vgmplay.ini not found; using %s\n", s_cfg_pick);
    return player_config_load(s_cfg_path);
}
