// vgz_inflate.h -- streaming gzip decompression for .vgz files. Decodes in
// bounded 1KB-in/32KB-window chunks (see vgz_inflate.c) so file size isn't
// limited by the RP2040's ~264KB of RAM; writes the result to dst_path on
// the SD card, which vgm_player then plays like any other .vgm file.
#pragma once

#include <stdbool.h>

bool vgz_inflate_file(const char *src_path, const char *dst_path);

// Peeks at the first 2 bytes of `path` for the gzip magic (0x1F 0x8B) --
// nothing is decompressed. Content, not the .vgm/.vgz extension, is what
// should decide whether a file needs inflating: some real-world VGM packs
// ship gzip-compressed data under a plain ".vgm" name (found via
// 調査用/Ashura-SMS/*.vgm, which decompress to valid single-SN76489 VGMs but
// were being read raw and rejected as "not a valid VGM file", silently
// aborting -- see main.c's play_one()). Returns false on any I/O error too
// (then the caller's plain-VGM path will fail its own header check instead,
// which is a normal "unsupported file" outcome, not a special case here).
bool vgz_looks_like_gzip(const char *path);
