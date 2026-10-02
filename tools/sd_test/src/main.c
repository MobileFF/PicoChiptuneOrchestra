// Standalone SD card bring-up / read test -- MASTER Pico only.
//
// Links the REAL SD wiring (master/src/hw_config.c) and the real FatFs_SPI
// library, exactly as the main firmware uses them -- not a reimplementation.
// No slaves, no multicore, no OLED/TFT, no vgm_player: just mount the card,
// list what's on it, and read a .vgm/.vgz file all the way through while
// timing it and summing its bytes, to answer "does SD card I/O actually
// work on this wiring" independent of anything else the real firmware does
// (including whether it shares SPI0 with an ST7735 TFT -- see
// docs/design-notes.md's TFT writeup; this test never touches a TFT at all,
// so if reads are flaky even HERE, the SD card/wiring itself is the
// suspect, not the TFT sharing the bus).
//
// Wiring: identical to the real firmware -- docs/circuit.md section 1.
//   MISO->GPIO16  CS->GPIO17  SCK->GPIO18  MOSI->GPIO19  (SPI0)
//
// How to use:
//   1. Flash sd_test.uf2 onto the MASTER Pico (hold BOOTSEL, copy, reboot).
//   2. Open the USB CDC serial port (baud rate is irrelevant for USB CDC).
//      A 5s countdown at boot gives you time to connect before the log starts.
//   3. Read the log. It repeats the whole pass every few seconds, so you can
//      watch for INTERMITTENT failures without resetting the board.
//
// Build: see tools/sd_test/README.md.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"

#include "spi0_bus_lock.h"

static bool has_extension(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + (nlen - elen), ext) == 0;
}

static bool is_vgm(const char *name) {
    return has_extension(name, ".vgm") || has_extension(name, ".vgz");
}

// Lists `dir_path`'s entries (name, DIR/size) and returns the full path of
// the first .vgm/.vgz file found directly in it, or an empty string. Also
// recurses ONE level into subdirectories (so "0:/ALLTEST/song.vgm" shows up
// even when nothing plays directly at the root) -- bounded, no need for
// main.c's full MAX_RECURSE_DEPTH machinery in a single-purpose test tool.
static void list_dir(const char *dir_path, char *first_vgm, size_t first_vgm_sz, int depth) {
    DIR dir;
    FILINFO info;
    FRESULT fr = f_findfirst(&dir, &info, dir_path, "*");
    if (fr != FR_OK) {
        printf("  %*s[could not list %s: FatFs error %d]\n", depth * 2, "", dir_path, fr);
        return;
    }
    int n = 0;
    while (info.fname[0] != 0) {
        n++;
        if (info.fattrib & AM_DIR) {
            printf("  %*s[DIR]  %s\n", depth * 2, "", info.fname);
            if (depth == 0) {
                char sub[260];
                snprintf(sub, sizeof(sub), "%s/%s", dir_path, info.fname);
                list_dir(sub, first_vgm, first_vgm_sz, depth + 1);
            }
        } else {
            printf("  %*s%-24s %10lu bytes%s\n", depth * 2, "", info.fname,
                   (unsigned long)info.fsize, is_vgm(info.fname) ? "  <- .vgm/.vgz" : "");
            if (is_vgm(info.fname) && first_vgm[0] == '\0') {
                snprintf(first_vgm, first_vgm_sz, "%s/%s", dir_path, info.fname);
            }
        }
        if (f_findnext(&dir, &info) != FR_OK) break;
    }
    f_closedir(&dir);
    if (n == 0) printf("  %*s(empty)\n", depth * 2, "");
}

// Reads `path` from start to EOF in 512-byte chunks, timing it and summing
// every byte (a trivial additive checksum -- not cryptographic, just enough
// to notice "the same file read twice gives different bytes", which a
// cryptographic hash wouldn't make any more obvious here).
static void read_through(const char *path) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        printf("  f_open(%s) failed: FatFs error %d\n", path, fr);
        return;
    }
    FSIZE_t size = f_size(&f);
    printf("  reading %s (%lu bytes)...\n", path, (unsigned long)size);

    static uint8_t buf[512];
    uint32_t checksum = 0;
    uint32_t total = 0;
    absolute_time_t start = get_absolute_time();
    for (;;) {
        UINT br = 0;
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK) {
            printf("  f_read failed after %lu bytes: FatFs error %d\n",
                   (unsigned long)total, fr);
            f_close(&f);
            return;
        }
        if (br == 0) break;
        for (UINT i = 0; i < br; i++) checksum += buf[i];
        total += br;
    }
    int64_t us = absolute_time_diff_us(start, get_absolute_time());
    f_close(&f);

    printf("  read %lu bytes in %lld ms (%.1f KB/s), checksum=0x%08lX\n",
           (unsigned long)total, (long long)(us / 1000),
           us > 0 ? (double)total * 1000000.0 / (double)us / 1024.0 : 0.0,
           (unsigned long)checksum);
    if (total != (uint32_t)size) {
        printf("  !! WARNING: read %lu bytes but f_size() said %lu -- short/long read\n",
               (unsigned long)total, (unsigned long)size);
    }
}

int main(void) {
    stdio_init_all();
    for (int s = 5; s > 0; s--) { printf("  starting in %d s ...\n", s); sleep_ms(1000); }

    printf("\n=== SD card standalone test ===\n");
    printf("SPI0  MISO=GPIO16 CS=GPIO17 SCK=GPIO18 MOSI=GPIO19\n");
    printf("================================\n\n");

    // glue.c's SD-card diskio functions unconditionally take this lock (see
    // spi0_bus_lock.h) -- nothing else uses SPI0 in this standalone test,
    // so it's never actually contended, but the mutex must still be
    // initialized before the first f_mount() takes it.
    spi0_bus_lock_init();

    uint32_t pass = 0;
    for (;;) {
        printf("---- pass %lu ----\n", (unsigned long)++pass);

        static FATFS fs;
        FRESULT fr = f_mount(&fs, "0:", 1);
        if (fr != FR_OK) {
            printf("f_mount FAILED: FatFs error %d (%s)\n", fr,
                   fr == FR_NOT_READY ? "FR_NOT_READY -- card not responding, check wiring/power"
                   : fr == FR_NO_FILESYSTEM ? "FR_NO_FILESYSTEM -- not FAT/FAT32, or card blank/corrupt"
                                            : "see ff.h's FRESULT enum");
            printf("retrying in 3 s...\n\n");
            sleep_ms(3000);
            continue;
        }
        printf("f_mount OK. Listing \"0:/\" (and one level into each subfolder):\n");

        char first_vgm[260] = {0};
        list_dir("0:", first_vgm, sizeof(first_vgm), 0);

        if (first_vgm[0]) {
            printf("\nReading the first .vgm/.vgz found:\n");
            read_through(first_vgm);
        } else {
            printf("\nNo .vgm/.vgz found (root or one level deep) -- nothing to read-test.\n");
        }

        f_unmount("0:");
        printf("\ndone, re-mounting in 3 s (Ctrl-C / reset to stop)...\n\n");
        sleep_ms(3000);
    }
}
