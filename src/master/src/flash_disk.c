#include "flash_disk.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024) // genuine Pico's onboard flash
#endif

// Reserve the LAST FLASH_DISK_BYTES of flash for this volume, regardless of
// how big the firmware image is -- simpler and safer than sizing this
// relative to the image (see __flash_binary_end check in flash_disk_init():
// the two are never allowed to overlap, checked at every boot rather than
// just assumed).
#define FLASH_DISK_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_DISK_BYTES)
#define SECTOR_SIZE 512u

_Static_assert(FLASH_DISK_BYTES % FLASH_SECTOR_SIZE == 0,
               "FLASH_DISK_BYTES must be a whole number of flash erase sectors");

static FATFS s_fatfs;
static bool s_ready; // flash_disk_init() succeeded this boot

// --- diskio backend -------------------------------------------------------
//
// FatFs on this project is built with FF_MIN_SS == FF_MAX_SS == 512 (see
// ffconf.h), so every disk_read/disk_write call here is in fixed 512-byte
// sectors regardless of the physical flash's 4096-byte erase granularity --
// disk_write below does the sector-to-erase-block translation (read the
// containing erase block, overlay the new bytes, erase, reprogram) so the
// FatFs side never needs to know flash erases in bigger chunks than it
// writes in.

DSTATUS flash_disk_diskio_status(void) { return s_ready ? 0 : STA_NOINIT; }
DSTATUS flash_disk_diskio_initialize(void) { return s_ready ? 0 : STA_NOINIT; }

DRESULT flash_disk_diskio_read(BYTE *buff, LBA_t sector, UINT count) {
    memcpy(buff, (const void *)(XIP_BASE + FLASH_DISK_OFFSET + (uint32_t)sector * SECTOR_SIZE),
           (size_t)count * SECTOR_SIZE);
    return RES_OK;
}

// One erase block (4096 bytes) at a time, off the heap/static storage, NOT
// core0's stack -- this project's own main.c comments document core0's stack
// as a mere 2KB (see docs/design-notes.md's HardFault writeup), so any
// buffer this size must be static regardless of which core calls in here.
static uint8_t s_erase_block[FLASH_SECTOR_SIZE];

DRESULT flash_disk_diskio_write(const BYTE *buff, LBA_t sector, UINT count) {
    uint32_t byte_off = (uint32_t)sector * SECTOR_SIZE;
    uint32_t byte_len = (uint32_t)count * SECTOR_SIZE;
    uint32_t blk_start = byte_off & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t blk_end = (byte_off + byte_len + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);

    // Flash erase/program stalls ALL XIP execution on BOTH cores (the flash
    // controller itself is exclusive, not per-core) -- core1 is normally
    // running oled_ui.c's render loop straight out of flash, so it must be
    // parked first (see multicore_lockout_victim_init() in oled_ui.c's
    // core1_main()) or it hangs/faults mid-fetch the instant erase starts.
    multicore_lockout_start_blocking();
    uint32_t irq = save_and_disable_interrupts();
    for (uint32_t blk = blk_start; blk < blk_end; blk += FLASH_SECTOR_SIZE) {
        memcpy(s_erase_block, (const void *)(XIP_BASE + FLASH_DISK_OFFSET + blk), FLASH_SECTOR_SIZE);
        uint32_t ov_start = (blk > byte_off) ? blk : byte_off;
        uint32_t ov_end_block = blk + FLASH_SECTOR_SIZE;
        uint32_t req_end = byte_off + byte_len;
        uint32_t ov_end = (ov_end_block < req_end) ? ov_end_block : req_end;
        if (ov_end > ov_start) {
            memcpy(s_erase_block + (ov_start - blk), buff + (ov_start - byte_off), ov_end - ov_start);
        }
        flash_range_erase(FLASH_DISK_OFFSET + blk, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_DISK_OFFSET + blk, s_erase_block, FLASH_SECTOR_SIZE);
    }
    restore_interrupts(irq);
    multicore_lockout_end_blocking();
    return RES_OK;
}

DRESULT flash_disk_diskio_ioctl(BYTE cmd, void *buff) {
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = FLASH_DISK_BYTES / SECTOR_SIZE; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = SECTOR_SIZE; return RES_OK;
        // f_mkfs aligns the data area on this many SECTORS -- one flash
        // erase block, so every cluster f_mkfs lays out starts on an erase
        // boundary (au_size below then makes every cluster exactly one
        // erase block too, so ordinary file writes rewrite whole blocks
        // cleanly instead of straddling two).
        case GET_BLOCK_SIZE: *(DWORD *)buff = FLASH_SECTOR_SIZE / SECTOR_SIZE; return RES_OK;
        default: return RES_PARERR;
    }
}

// --- mount/format + the actual cache-a-song helper ------------------------

bool flash_disk_init(void) {
    extern char __flash_binary_end;
    uint32_t bin_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    if (bin_end > FLASH_DISK_OFFSET) {
        printf("flash cache: DISABLED -- firmware (%lu bytes) has grown into the "
               "reserved cache region (offset 0x%lX); bump PICO_FLASH_SIZE_BYTES's "
               "budget in flash_disk.h or shrink FLASH_DISK_BYTES\n",
               (unsigned long)bin_end, (unsigned long)FLASH_DISK_OFFSET);
        return false;
    }

    s_ready = true; // disk_status/initialize below need this before f_mount probes them
    FRESULT fr = f_mount(&s_fatfs, "1:", 1);
    if (fr != FR_OK) {
        // Not a valid FAT volume yet -- a fresh board, or the first boot
        // after this feature shipped. Format once; a cluster == one flash
        // erase block (see GET_BLOCK_SIZE above) keeps ordinary writes from
        // straddling two erase blocks at once.
        printf("flash cache: formatting %u KB flash region for the first time...\n",
               FLASH_DISK_BYTES / 1024);
        static BYTE work[FF_MAX_SS];
        MKFS_PARM opt = {.fmt = FM_FAT, .n_fat = 1, .align = 0, .n_root = 16,
                          .au_size = FLASH_SECTOR_SIZE};
        fr = f_mkfs("1:", &opt, work, sizeof(work));
        if (fr != FR_OK) {
            printf("flash cache: DISABLED -- f_mkfs failed (%d)\n", fr);
            s_ready = false;
            return false;
        }
        fr = f_mount(&s_fatfs, "1:", 1);
        if (fr != FR_OK) {
            printf("flash cache: DISABLED -- mount after format failed (%d)\n", fr);
            s_ready = false;
            return false;
        }
    }
    printf("flash cache: \"1:\" mounted (%u KB at flash offset 0x%X)\n",
           FLASH_DISK_BYTES / 1024, FLASH_DISK_OFFSET);
    return true;
}

bool flash_disk_cache_file(const char *src_path, char *out_path, size_t out_path_sz) {
    if (!s_ready) return false;
    if (out_path_sz < sizeof(FLASH_DISK_CACHE_PATH)) return false;

    FIL src;
    if (f_open(&src, src_path, FA_READ) != FR_OK) return false;
    FSIZE_t size = f_size(&src);
    // A little headroom below the raw volume size for filesystem overhead
    // (FAT table, root directory, the one file's own directory entry) --
    // f_write() below would just fail partway on a file that's actually too
    // big anyway, but this catches the common case up front without ever
    // touching flash.
    if (size > FLASH_DISK_BYTES - 16384) {
        f_close(&src);
        printf("flash cache: %s is %lu bytes, too big for the %u KB cache -- "
               "streaming from its original location instead\n",
               src_path, (unsigned long)size, FLASH_DISK_BYTES / 1024);
        return false;
    }

    FIL dst;
    if (f_open(&dst, FLASH_DISK_CACHE_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        f_close(&src);
        printf("flash cache: could not create %s\n", FLASH_DISK_CACHE_PATH);
        return false;
    }

    static uint8_t buf[512]; // static: see the core0-stack note above
    bool ok = true;
    for (;;) {
        UINT br = 0;
        if (f_read(&src, buf, sizeof(buf), &br) != FR_OK) { ok = false; break; }
        if (br == 0) break;
        UINT bw = 0;
        if (f_write(&dst, buf, br, &bw) != FR_OK || bw != br) { ok = false; break; }
    }
    f_close(&src);
    f_close(&dst);
    if (!ok) {
        printf("flash cache: copy of %s failed partway, streaming from its "
               "original location instead\n", src_path);
        return false;
    }
    memcpy(out_path, FLASH_DISK_CACHE_PATH, sizeof(FLASH_DISK_CACHE_PATH));
    return true;
}
