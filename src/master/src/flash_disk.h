// flash_disk.h -- a second FatFs volume ("1:") backed by a fixed region of
// the master's own onboard flash, used as a single-file scratch cache so a
// song can be copied there once at song-start and then read back via XIP
// (memory-mapped flash) for the rest of playback, instead of streaming every
// byte from the SD card over SPI0 for the whole song.
//
// Why: this master's SD card and an eventual SPI-connected status display
// would otherwise have to share SPI0's physical wires for the whole song,
// needing careful bus-arbitration between the two. Once a song is copied
// here, the SD card sits idle for the rest of that song, so SPI0 is free for
// the display without any locking against SD I/O -- see docs/design-notes.md
// for the fuller discussion of the tradeoff this took over PIO/mutex options.
//
// Same idea as the existing gzip-decompression path (vgz_inflate.c writes a
// decompressed copy to "0:/_vgztmp.vgm" and plays from THAT path) -- just
// targeting flash instead of the SD card, so vgm_player.c needs no changes
// at all: it already just takes a FatFs path.
//
// Opt-in via vgmplay.ini's [player] flash_cache = yes (default no, see
// player_config.h) -- new, unproven-on-hardware functionality, and writing
// to flash at all is a bigger deal than any other [player] setting so far.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "ff.h"     // BYTE/UINT/LBA_t (diskio.h depends on these, like glue.c does)
#include "diskio.h" // DSTATUS/DRESULT -- see FatFs_SPI/ff15/source

// Physical drive number this volume is mounted on (glue.c dispatches pdrv ==
// this value here instead of to the SD card; see FatFs_SPI/src/glue.c).
#define FLASH_DISK_PDRV 1

// --- diskio backend for FLASH_DISK_PDRV, called only from glue.c's
// disk_status/disk_initialize/disk_read/disk_write/disk_ioctl once they see
// pdrv == FLASH_DISK_PDRV. Not meant to be called from anywhere else in the
// master's own code -- use flash_disk_init()/flash_disk_cache_file() below.
DSTATUS flash_disk_diskio_status(void);
DSTATUS flash_disk_diskio_initialize(void);
DRESULT flash_disk_diskio_read(BYTE *buff, LBA_t sector, UINT count);
DRESULT flash_disk_diskio_write(const BYTE *buff, LBA_t sector, UINT count);
DRESULT flash_disk_diskio_ioctl(BYTE cmd, void *buff);

// Size of the reserved flash region (the whole "1:" volume, filesystem
// overhead included) -- see vgmplay_config_gui.py / docs/design-notes.md for
// why 1 MiB: current firmware is ~110 KB and every sample VGM on hand is
// under 700 KB, so this comfortably covers real songs with room to spare on
// a 2 MB (original Pico) or 4 MB (Pico 2) flash chip alike.
#define FLASH_DISK_BYTES (1024u * 1024u)

// FatFs path of the one scratch file main.c copies each song into and plays
// back from.
#define FLASH_DISK_CACHE_PATH "1:/cache.vgm"

// Reserve+mount "1:" on the flash region above, formatting it first if it
// isn't already a valid FAT volume (a fresh board, or the first boot after
// this feature shipped). Call once at boot, after the SD card is mounted,
// only when [player] flash_cache is enabled -- pointless flash wear
// otherwise. Returns false (logged) if the firmware image has grown too big
// to leave room for this region, or formatting/mounting failed; the caller
// should then just never call flash_disk_cache_file() this boot.
bool flash_disk_init(void);

// Copy `src_path` (a FatFs path, e.g. "0:/song.vgm") into the flash cache's
// one scratch file and, on success, write FLASH_DISK_CACHE_PATH into
// `out_path` (must be at least FLASH_DISK_CACHE_PATH's length + 1 bytes) for
// the caller to play from instead of src_path. Returns false (out_path left
// untouched) when flash_disk_init() never succeeded, src_path is larger than
// this volume can hold, or the copy itself fails partway -- in every false
// case the caller should just keep playing src_path directly, exactly as if
// flash_cache were off.
bool flash_disk_cache_file(const char *src_path, char *out_path, size_t out_path_sz);
