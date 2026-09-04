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
    int cur = -1; // current [chip] section, -1 = none/unknown
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
            cur = lookup_chip(name);
            if (cur < 0) printf("config: unknown chip section [%s], skipping its keys\n", name);
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
        } else {
            printf("config: unknown key '%s', ignored\n", key);
        }
    }
    return applied;
}

int player_config_load(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("config: no %s on card, using built-in defaults\n", path);
        return 0;
    }
    static char buf[4096];
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
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
    FIL probe;
    if (f_open(&probe, "0:/vgmplay.ini", FA_READ) == FR_OK) {
        f_close(&probe);
        return player_config_load("0:/vgmplay.ini");
    }

    // Otherwise the alphabetically-first "vgmplay*.ini" in the root, so a
    // card can carry e.g. "vgmplay_scc.ini" and it's obvious at a glance
    // which chip set that card is wired for. Sorted (not raw dir order) so
    // the choice is deterministic when several are present.
    DIR dir;
    FILINFO info;
    char pick[256]; // matches FILINFO.fname when long filenames are enabled
    pick[0] = '\0';
    if (f_findfirst(&dir, &info, "0:", "*") == FR_OK) {
        while (info.fname[0]) {
            if (!(info.fattrib & AM_DIR) &&
                strncasecmp(info.fname, "vgmplay", 7) == 0 &&
                ci_ends_with(info.fname, ".ini") &&
                (pick[0] == '\0' || strcasecmp(info.fname, pick) < 0)) {
                snprintf(pick, sizeof(pick), "%s", info.fname);
            }
            if (f_findnext(&dir, &info) != FR_OK) break;
        }
        f_closedir(&dir);
    }

    if (pick[0] == '\0') {
        printf("config: no vgmplay*.ini on card, using built-in defaults\n");
        return 0;
    }
    char path[4 + sizeof(pick)];
    snprintf(path, sizeof(path), "0:/%s", pick);
    printf("config: vgmplay.ini not found; using %s\n", pick);
    return player_config_load(path);
}
