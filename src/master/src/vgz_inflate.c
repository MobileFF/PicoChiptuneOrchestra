#include "vgz_inflate.h"

#include "ff.h"
#include "miniz_tinfl.h"

static bool skip_cstring(FIL *f) {
    UINT br;
    uint8_t c;
    do {
        if (f_read(f, &c, 1, &br) != FR_OK || br != 1) return false;
    } while (c != 0);
    return true;
}

static bool skip_gzip_header(FIL *f) {
    uint8_t hdr[10];
    UINT br;
    if (f_read(f, hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr)) return false;
    if (hdr[0] != 0x1F || hdr[1] != 0x8B || hdr[2] != 8) return false; // not gzip/deflate
    uint8_t flg = hdr[3];

    if (flg & 0x04) { // FEXTRA
        uint8_t xl[2];
        if (f_read(f, xl, 2, &br) != FR_OK || br != 2) return false;
        uint16_t xlen = (uint16_t)(xl[0] | (xl[1] << 8));
        if (f_lseek(f, f_tell(f) + xlen) != FR_OK) return false;
    }
    if (flg & 0x08) { // FNAME
        if (!skip_cstring(f)) return false;
    }
    if (flg & 0x10) { // FCOMMENT
        if (!skip_cstring(f)) return false;
    }
    if (flg & 0x02) { // FHCRC
        if (f_lseek(f, f_tell(f) + 2) != FR_OK) return false;
    }
    return true;
}

bool vgz_inflate_file(const char *src_path, const char *dst_path) {
    FIL fsrc, fdst;
    if (f_open(&fsrc, src_path, FA_READ) != FR_OK) return false;
    if (!skip_gzip_header(&fsrc)) {
        f_close(&fsrc);
        return false;
    }
    if (f_open(&fdst, dst_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_close(&fsrc);
        return false;
    }

    static uint8_t s_dict[TINFL_LZ_DICT_SIZE];
    static uint8_t s_in[1024];
    tinfl_decompressor decomp;
    tinfl_init(&decomp);

    size_t in_avail = 0, in_pos = 0;
    bool src_eof = false;
    uint32_t dict_ofs = 0;
    bool ok = true;

    for (;;) {
        if (in_pos == in_avail && !src_eof) {
            UINT br = 0;
            if (f_read(&fsrc, s_in, sizeof(s_in), &br) != FR_OK) { ok = false; break; }
            in_avail = br;
            in_pos = 0;
            if (br == 0) src_eof = true;
        }

        size_t in_buf_size = in_avail - in_pos;
        size_t out_buf_size = TINFL_LZ_DICT_SIZE - dict_ofs;
        uint32_t flags = src_eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT;

        tinfl_status st = tinfl_decompress(&decomp, s_in + in_pos, &in_buf_size,
                                            s_dict, s_dict + dict_ofs, &out_buf_size, flags);
        in_pos += in_buf_size;

        if (out_buf_size > 0) {
            UINT bw = 0;
            if (f_write(&fdst, s_dict + dict_ofs, out_buf_size, &bw) != FR_OK || bw != out_buf_size) {
                ok = false;
                break;
            }
            dict_ofs = (dict_ofs + (uint32_t)out_buf_size) & (TINFL_LZ_DICT_SIZE - 1);
        }

        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) { ok = false; break; }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_pos == in_avail && src_eof) {
            ok = false; // truncated stream
            break;
        }
    }

    f_close(&fsrc);
    f_close(&fdst);
    return ok;
}
