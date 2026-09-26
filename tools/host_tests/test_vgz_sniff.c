// Verifies master/src/vgz_inflate.c's vgz_looks_like_gzip(): a pure 2-byte
// magic sniff (0x1F 0x8B), independent of whether the rest of the file is
// valid gzip/deflate data or even present. This is what main.c's play_one()
// uses to decide whether to decompress a file BY CONTENT rather than by its
// .vgm/.vgz extension -- see its own comment for the real-world case that
// motivated this (調査用/Ashura-SMS/*.vgm: gzip-compressed VGM data shipped
// under a plain ".vgm" name, previously read raw and silently rejected as
// "not a valid VGM file"). Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../third_party/miniz_tinfl
//   test_vgz_sniff.c ../../src/master/src/vgz_inflate.c
//   ../../third_party/miniz_tinfl/miniz_tinfl.c -o /tmp/vgz_sniff_test
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "vgz_inflate.h"

static void write_file(const char *path, const uint8_t *data, size_t n) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

int main(void) {
    int fail = 0;
    #define CHK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); fail = 1; } } while (0)

    const uint8_t gzip_like[] = {0x1F, 0x8B, 0x08, 0x00, 'x', 'x', 'x'};
    write_file("/tmp/sniff_gzip.bin", gzip_like, sizeof(gzip_like));
    CHK(vgz_looks_like_gzip("/tmp/sniff_gzip.bin") == true);

    // A real VGM header -- must NOT be mistaken for gzip.
    const uint8_t vgm_like[] = {'V', 'g', 'm', ' ', 0, 0, 0, 0};
    write_file("/tmp/sniff_vgm.bin", vgm_like, sizeof(vgm_like));
    CHK(vgz_looks_like_gzip("/tmp/sniff_vgm.bin") == false);

    // Only the first byte of the magic present -- too short, not a match.
    const uint8_t one_byte[] = {0x1F};
    write_file("/tmp/sniff_short.bin", one_byte, sizeof(one_byte));
    CHK(vgz_looks_like_gzip("/tmp/sniff_short.bin") == false);

    // Empty file.
    write_file("/tmp/sniff_empty.bin", NULL, 0);
    CHK(vgz_looks_like_gzip("/tmp/sniff_empty.bin") == false);

    // Missing file -- an I/O error, not a match.
    CHK(vgz_looks_like_gzip("/tmp/sniff_does_not_exist.bin") == false);

    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
