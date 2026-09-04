// Host-side stand-in for master/src/vgz_inflate.c, with FatFs f_* calls
// swapped for stdio, to validate the streaming tinfl loop logic against a
// real gzip file without needing RP2040 hardware.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "miniz_tinfl.h"

static bool skip_cstring(FILE *f) {
    int c;
    do { c = fgetc(f); if (c == EOF) return false; } while (c != 0);
    return true;
}

static bool skip_gzip_header(FILE *f) {
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10) return false;
    if (hdr[0] != 0x1F || hdr[1] != 0x8B || hdr[2] != 8) return false;
    uint8_t flg = hdr[3];
    if (flg & 0x04) {
        uint8_t xl[2];
        if (fread(xl, 1, 2, f) != 2) return false;
        uint16_t xlen = (uint16_t)(xl[0] | (xl[1] << 8));
        fseek(f, xlen, SEEK_CUR);
    }
    if (flg & 0x08) { if (!skip_cstring(f)) return false; }
    if (flg & 0x10) { if (!skip_cstring(f)) return false; }
    if (flg & 0x02) { fseek(f, 2, SEEK_CUR); }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s in.gz out\n", argv[0]); return 2; }
    FILE *fsrc = fopen(argv[1], "rb");
    FILE *fdst = fopen(argv[2], "wb");
    if (!fsrc || !fdst) { fprintf(stderr, "open failed\n"); return 2; }
    if (!skip_gzip_header(fsrc)) { fprintf(stderr, "bad gzip header\n"); return 1; }

    static uint8_t s_dict[TINFL_LZ_DICT_SIZE];
    static uint8_t s_in[1024];
    tinfl_decompressor decomp;
    tinfl_init(&decomp);

    size_t in_avail = 0, in_pos = 0;
    bool src_eof = false;
    uint32_t dict_ofs = 0;
    bool ok = true;
    long total_out = 0;

    for (;;) {
        if (in_pos == in_avail && !src_eof) {
            size_t br = fread(s_in, 1, sizeof(s_in), fsrc);
            in_avail = br; in_pos = 0;
            if (br == 0) src_eof = true;
        }
        size_t in_buf_size = in_avail - in_pos;
        size_t out_buf_size = TINFL_LZ_DICT_SIZE - dict_ofs;
        uint32_t flags = src_eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT;

        tinfl_status st = tinfl_decompress(&decomp, s_in + in_pos, &in_buf_size,
                                            s_dict, s_dict + dict_ofs, &out_buf_size, flags);
        in_pos += in_buf_size;

        if (out_buf_size > 0) {
            size_t bw = fwrite(s_dict + dict_ofs, 1, out_buf_size, fdst);
            if (bw != out_buf_size) { ok = false; break; }
            total_out += (long)bw;
            dict_ofs = (dict_ofs + (uint32_t)out_buf_size) & (TINFL_LZ_DICT_SIZE - 1);
        }

        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) { fprintf(stderr, "tinfl error %d\n", st); ok = false; break; }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_pos == in_avail && src_eof) {
            fprintf(stderr, "truncated stream\n"); ok = false; break;
        }
    }

    fclose(fsrc); fclose(fdst);
    fprintf(stderr, "ok=%d total_out=%ld\n", ok, total_out);
    return ok ? 0 : 1;
}
