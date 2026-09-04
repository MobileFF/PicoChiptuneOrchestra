// vgz_inflate.h -- streaming gzip decompression for .vgz files. Decodes in
// bounded 1KB-in/32KB-window chunks (see vgz_inflate.c) so file size isn't
// limited by the RP2040's ~264KB of RAM; writes the result to dst_path on
// the SD card, which vgm_player then plays like any other .vgm file.
#pragma once

#include <stdbool.h>

bool vgz_inflate_file(const char *src_path, const char *dst_path);
