// inflate_scratch.h -- the one shared tinfl (miniz_tinfl) scratch area for
// every raw-DEFLATE consumer in this firmware: vgz_inflate.c's gzip
// decompression and cover_image.c's PNG IDAT decompression. Both are
// core0-only, one-shot-per-file operations that never run concurrently (a
// song's .vgz is inflated before playback starts; a folder's cover PNG is
// decoded once per folder, also before playback) and each needs ~40KB
// (an 8KB tinfl_decompressor plus a 32KB TINFL_LZ_DICT_SIZE sliding window)
// -- sharing one instance instead of each having its own saves that ~40KB
// of duplicate static RAM.
#pragma once

#include "miniz_tinfl.h"

// Both MUST be static storage, never stack-local: see vgz_inflate.c's own
// comment on why (an ~8KB decompressor on core0's 2KB stack once corrupted
// core1's adjacent stack and caused a hard-to-diagnose HardFault).
tinfl_decompressor *inflate_scratch_decomp(void);
uint8_t *inflate_scratch_dict(void); // TINFL_LZ_DICT_SIZE bytes
