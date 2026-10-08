#include "cover_image.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ff.h"
#include "tjpgd.h"
#include "miniz_tinfl.h"
#include "inflate_scratch.h"
#include "tft_panel.h"
#include "player_config.h"
#include "spi0_bus_lock.h"

// Cover-art area size for the configured [player] tft_panel (see tft_panel.h).
static int cover_w(void) { return tft_panel_geom()->w; }
static int cover_h(void) { return tft_panel_geom()->cover_h; }

static bool has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen) return false;
    return strcasecmp(name + (nlen - elen), ext) == 0;
}

bool cover_image_is_supported(const char *name) {
    return has_ext(name, ".jpg") || has_ext(name, ".jpeg") || has_ext(name, ".png");
}

// --- shared scale-to-fit math ----------------------------------------------
//
// Uniform integer decimation (same factor both axes, so the image is only
// ever shrunk, never distorted) so `src_w` fits within cover_w(). Never
// upscales (returns 1 for a source already <= cover_w()). A source
// that's still taller than cover_h() after this -- a tall/portrait photo
// -- is centred and silently clipped top/bottom by tft_cover_blit()'s own
// bounds check, rather than shrunk further to fit height too: this matches
// "fit to the display WIDTH" as asked for.

static int fit_decimation(int src_w) {
    int d = (src_w + cover_w() - 1) / cover_w();
    return d < 1 ? 1 : d;
}

// ============================================================================
// JPEG (baseline, via third_party/tjpgd -- ChaN's TJpgDec)
// ============================================================================

typedef struct {
    FIL *fp;
    int extra;        // additional nearest-neighbour decimation beyond tjpgd's own 1/2^N scale
    int dst_x0, dst_y0; // top-left of the final (both-scaled) image within the cover area
} jpeg_ctx_t;

// Signatures must match tjpgd.h's infunc/outfunc typedefs exactly (uint16_t,
// not size_t/int -- this is an 8/16-bit-MCU-era API).
static uint16_t jpeg_in(JDEC *jd, uint8_t *buf, uint16_t nbyte) {
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!buf) { // NULL buf means "skip nbyte bytes of input", per tjpgd's own convention
        f_lseek(ctx->fp, f_tell(ctx->fp) + nbyte);
        return nbyte;
    }
    UINT br = 0;
    if (f_read(ctx->fp, buf, nbyte, &br) != FR_OK) return 0;
    return (uint16_t)br;
}

// Receives one decoded MCU rectangle at a time (RGB565, native-endian
// uint16_t -- see third_party/tjpgd/tjpgd.h's JD_FORMAT comment) and blits
// the kept (post-decimation) pixels straight into the TFT framebuffer/panel,
// one destination row at a time. No full-image buffer needed.
static uint16_t jpeg_out(JDEC *jd, void *bitmap, JRECT *rect) {
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    int rw = rect->right - rect->left + 1;
    static uint16_t row[TFT_MAX_W];

    for (int sy = rect->top; sy <= rect->bottom; sy++) {
        if (sy % ctx->extra != 0) continue;
        int dy = ctx->dst_y0 + sy / ctx->extra;
        int n = 0, first_dx = -1;
        for (int sx = rect->left; sx <= rect->right; sx++) {
            if (sx % ctx->extra != 0) continue;
            int dx = ctx->dst_x0 + sx / ctx->extra;
            if (first_dx < 0) first_dx = dx;
            if ((size_t)n < sizeof(row) / sizeof(row[0]))
                row[n++] = src[(sy - rect->top) * rw + (sx - rect->left)];
        }
        if (n > 0) tft_cover_blit(first_dx, dy, n, 1, row);
    }
    return 1; // continue decoding
}

static bool decode_jpeg(FIL *fp) {
    // TJpgDec's own appnote: "requires a work area upto 3100 bytes for most
    // JPEG images" -- a little headroom over that. MUST be static: this and
    // JDEC together are a few KB, far past core0's 2KB stack budget (see
    // vgz_inflate.c's own comment on exactly this class of bug).
    static uint8_t work[4096];
    static JDEC jd;
    jpeg_ctx_t ctx = {.fp = fp, .extra = 1, .dst_x0 = 0, .dst_y0 = 0};

    if (jd_prepare(&jd, jpeg_in, work, sizeof(work), &ctx) != JDR_OK) return false;

    // Pick tjpgd's own coarse 1/2^N descale (N=0..3) so the pre-blit image is
    // already close to (but not below) cover_w() wide, minimizing the
    // extra nearest-neighbour decimation jpeg_out() still has to do to land
    // on the exact target (tjpgd only offers powers of 2, real cover art is
    // rarely a power-of-2 multiple of 128px).
    uint8_t scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= cover_w()) scale++;

    int decoded_w = jd.width >> scale;
    int decoded_h = jd.height >> scale;
    ctx.extra = fit_decimation(decoded_w);
    int final_w = decoded_w / ctx.extra;
    int final_h = decoded_h / ctx.extra;
    ctx.dst_x0 = (cover_w() - final_w) / 2;
    ctx.dst_y0 = (cover_h() - final_h) / 2;

    return jd_decomp(&jd, jpeg_out, scale) == JDR_OK;
}

// ============================================================================
// PNG (hand-rolled: chunk parsing + scanline defiltering here, DEFLATE via
// the shared tinfl instance in inflate_scratch.h -- same decompressor
// vgz_inflate.c uses for .vgz, never needed concurrently with that).
//
// Deliberately supports only: bit depth 8 (color types 0/2/3/4/6 --
// grayscale, RGB, palette, grayscale+alpha, RGBA) or, for palette (color
// type 3) specifically, also 1/2/4 bits/pixel (small palettes are very
// often packed smaller than 8 bits/pixel -- see png_palette_index()'s
// comment), non-interlaced. NOT supported: 16-bit depth, sub-8-bit
// grayscale/RGB/etc., Adam7 interlacing. This covers the large majority of
// real-world PNG exports from photo/image tools (and, importantly, a lot of
// retro game cover art -- palette PNGs are common there); anything else
// just logs and falls back to a blank cover area (see cover_image_show()),
// same as a missing image -- never a crash or garbage render.
// ============================================================================

#define MAX_PNG_SRC_W 1024  // bounds scanline RAM use (see row buffers below)
#define MAX_PNG_SRC_H 4096  // sanity cap against a corrupt/hostile height field
#define MAX_PNG_ROW_BYTES (MAX_PNG_SRC_W * 4) // worst case: color type 6 (RGBA), bpp=4

// Color type 3 (palette)'s PLTE chunk: up to 256 entries, 3 bytes (R,G,B)
// each, always before the first IDAT -- see decode_png()'s chunk-skip loop.
// static: core0 stack discipline, same reasoning as every other PNG buffer
// in this file.
#define MAX_PNG_PALETTE_ENTRIES 256
static uint8_t s_png_palette[MAX_PNG_PALETTE_ENTRIES * 3];

// Demultiplexes the PNG chunk framing, handing back only concatenated IDAT
// chunk DATA bytes (chunk length/type/CRC headers and any chunks after the
// IDAT run are transparently skipped/stop iteration).
typedef struct {
    FIL *fp;
    uint32_t chunk_remaining; // bytes left in the CURRENT IDAT chunk's data
    bool eof;                 // hit a non-IDAT chunk, or a read error/truncation
} idat_reader_t;

static size_t idat_read(idat_reader_t *r, uint8_t *buf, size_t want) {
    size_t got = 0;
    while (got < want && !r->eof) {
        if (r->chunk_remaining == 0) {
            uint8_t crc[4], h[8];
            UINT br;
            if (f_read(r->fp, crc, 4, &br) != FR_OK || br != 4) { r->eof = true; break; }
            if (f_read(r->fp, h, 8, &br) != FR_OK || br != 8) { r->eof = true; break; }
            if (memcmp(h + 4, "IDAT", 4) != 0) { r->eof = true; break; } // IEND or another chunk type: done
            r->chunk_remaining = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                                  ((uint32_t)h[2] << 8) | h[3];
        }
        UINT want_now = (UINT)((want - got) < r->chunk_remaining ? (want - got) : r->chunk_remaining);
        UINT br = 0;
        if (f_read(r->fp, buf + got, want_now, &br) != FR_OK) { r->eof = true; break; }
        got += br;
        r->chunk_remaining -= br;
        if (br < want_now) { r->eof = true; break; } // short read -- truncated file
    }
    return got;
}

// Pulls fixed-size chunks out of the tinfl output stream on demand (PNG
// needs exact byte counts -- one filter byte, then one scanline's worth --
// which rarely line up with whatever size tinfl happens to produce per
// call), fed by idat_read() above. `pending_*` tracks decompressed bytes
// tinfl has already produced but a previous read_bytes() call didn't need
// yet; they're drained before asking tinfl for more, so they're always
// consumed well before the 32KB dict could wrap around onto them.
typedef struct {
    idat_reader_t *idat;
    tinfl_decompressor *decomp;
    uint8_t *dict;
    uint32_t dict_ofs;     // next position tinfl will PRODUCE into
    size_t pending_start;  // start of not-yet-consumed output already in dict[]
    size_t pending_len;
    uint8_t in_buf[512];
    size_t in_avail, in_pos;
} png_inflate_t;

static void png_inflate_init(png_inflate_t *pi, idat_reader_t *idat) {
    pi->idat = idat;
    pi->decomp = inflate_scratch_decomp();
    pi->dict = inflate_scratch_dict();
    tinfl_init(pi->decomp);
    pi->dict_ofs = 0;
    pi->pending_start = pi->pending_len = 0;
    pi->in_avail = pi->in_pos = 0;
}

static bool png_inflate_read(png_inflate_t *pi, uint8_t *out, size_t want) {
    size_t got = 0;
    while (got < want) {
        if (pi->pending_len > 0) {
            size_t take = pi->pending_len < (want - got) ? pi->pending_len : (want - got);
            memcpy(out + got, pi->dict + pi->pending_start, take);
            pi->pending_start += take;
            pi->pending_len -= take;
            got += take;
            continue;
        }
        if (pi->in_pos == pi->in_avail) {
            pi->in_avail = idat_read(pi->idat, pi->in_buf, sizeof(pi->in_buf));
            pi->in_pos = 0;
        }
        size_t in_buf_size = pi->in_avail - pi->in_pos;
        size_t out_buf_size = TINFL_LZ_DICT_SIZE - pi->dict_ofs;
        // PNG's IDAT stream is zlib-wrapped (RFC 1950: a 2-byte header, then
        // raw DEFLATE, then a 4-byte Adler32 trailer) -- unlike
        // vgz_inflate.c's gzip stream, which strips its own (different)
        // wrapper by hand before ever calling tinfl, PNG's zlib wrapper is
        // simple enough that tinfl can just be told to parse it itself.
        uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (pi->idat->eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT);

        tinfl_status st = tinfl_decompress(pi->decomp, pi->in_buf + pi->in_pos, &in_buf_size,
                                            pi->dict, pi->dict + pi->dict_ofs, &out_buf_size, flags);
        pi->in_pos += in_buf_size;

        if (out_buf_size > 0) {
            pi->pending_start = pi->dict_ofs;
            pi->pending_len = out_buf_size;
            pi->dict_ofs = (pi->dict_ofs + (uint32_t)out_buf_size) & (TINFL_LZ_DICT_SIZE - 1);
            continue;
        }
        if (st == TINFL_STATUS_DONE) return false;  // stream ended before `want` was satisfied
        if (st < 0) return false;                    // decompression error
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && pi->in_pos == pi->in_avail && pi->idat->eof)
            return false; // truncated
        // else: loop and try again (e.g. needs more input but more IDAT data remains)
    }
    return true;
}

// Reverses one scanline's PNG filter in place. `cur` holds the just-read raw
// bytes; `prev` is the previous row's already-reconstructed bytes (all-zero
// for the image's first row, per the PNG spec -- see decode_png()).
static void png_unfilter(uint8_t filter_type, uint8_t *cur, const uint8_t *prev, size_t n, int bpp) {
    for (size_t i = 0; i < n; i++) {
        int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
        int b = prev[i];
        int c = (i >= (size_t)bpp) ? prev[i - bpp] : 0;
        int raw = cur[i], recon;
        switch (filter_type) {
            case 1: recon = raw + a; break;                 // Sub
            case 2: recon = raw + b; break;                 // Up
            case 3: recon = raw + (a + b) / 2; break;        // Average
            case 4: {                                        // Paeth
                int p = a + b - c;
                int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                recon = raw + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                break;
            }
            default: recon = raw; break;                     // None (0), or an unknown type
        }
        cur[i] = (uint8_t)recon;
    }
}

// Palette (color type 3) pixels can be packed at 1/2/4/8 bits each (PNG
// allows all four for indexed images -- a small palette very often gets
// encoded at less than 8 bits/pixel to save space, as the actual image
// that prompted this did: 16-colour palette -> 4 bits/pixel). Bits are
// packed MSB-first within each byte, left to right, per the spec -- this
// one formula covers all four depths (at bit_depth==8, pixels_per_byte==1
// and the shift/mask reduce to a plain byte read).
static uint8_t png_palette_index(const uint8_t *row, uint32_t x, int bit_depth) {
    uint32_t pixels_per_byte = 8u / (uint32_t)bit_depth;
    uint8_t byte = row[x / pixels_per_byte];
    uint32_t shift = (pixels_per_byte - 1 - (x % pixels_per_byte)) * (uint32_t)bit_depth;
    uint8_t mask = (uint8_t)((1u << bit_depth) - 1);
    return (uint8_t)((byte >> shift) & mask);
}

// `palette`: non-NULL for color type 3 (indexed) rows, where each "pixel"
// in `row` is a palette index (possibly sub-byte-packed, see
// png_palette_index() above) rather than actual sample bytes -- NULL for
// every other supported color type (bpp alone tells them apart). `bit_depth`
// only matters when `palette` is non-NULL (every other supported color
// type is bit_depth-8-only).
static void png_blit_row(const uint8_t *row, uint32_t width, int bpp, int bit_depth, int dy, int extra, int dst_x0,
                          const uint8_t *palette) {
    static uint16_t out_row[TFT_MAX_W];
    int n = 0, first_dx = -1;
    for (uint32_t sx = 0; sx < width; sx += (uint32_t)extra) {
        uint8_t r, g, b;
        if (palette) {
            uint8_t idx = png_palette_index(row, sx, bit_depth);
            const uint8_t *pal = palette + (size_t)idx * 3; // idx: 0-255, palette has 256 slots -- always in bounds
            r = pal[0]; g = pal[1]; b = pal[2];
        } else {
            const uint8_t *px = row + (size_t)sx * (size_t)bpp;
            if (bpp <= 2) { r = g = b = px[0]; }              // grayscale, or grayscale+alpha (alpha ignored)
            else          { r = px[0]; g = px[1]; b = px[2]; } // RGB or RGBA (alpha ignored)
        }
        uint16_t color = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        int dx = dst_x0 + (int)(sx / (uint32_t)extra);
        if (first_dx < 0) first_dx = dx;
        if ((size_t)n < sizeof(out_row) / sizeof(out_row[0])) out_row[n++] = color;
    }
    if (n > 0) tft_cover_blit(first_dx, dy, n, 1, out_row);
}

static bool decode_png(FIL *fp) {
    static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t sig[8];
    UINT br;
    if (f_read(fp, sig, 8, &br) != FR_OK || br != 8 || memcmp(sig, PNG_SIG, 8) != 0) return false;

    uint8_t h[8];
    if (f_read(fp, h, 8, &br) != FR_OK || br != 8 || memcmp(h + 4, "IHDR", 4) != 0) return false;
    uint32_t ihdr_len = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    uint8_t ihdr[13];
    if (ihdr_len != sizeof(ihdr) || f_read(fp, ihdr, sizeof(ihdr), &br) != FR_OK || br != sizeof(ihdr))
        return false;
    if (f_lseek(fp, f_tell(fp) + 4) != FR_OK) return false; // skip IHDR's CRC

    uint32_t width  = ((uint32_t)ihdr[0] << 24) | ((uint32_t)ihdr[1] << 16) | ((uint32_t)ihdr[2] << 8) | ihdr[3];
    uint32_t height = ((uint32_t)ihdr[4] << 24) | ((uint32_t)ihdr[5] << 16) | ((uint32_t)ihdr[6] << 8) | ihdr[7];
    uint8_t bit_depth = ihdr[8], color_type = ihdr[9], interlace = ihdr[12];

    int bpp;
    switch (color_type) {
        case 0: bpp = 1; break; // grayscale
        case 2: bpp = 3; break; // RGB
        case 3: bpp = 1; break; // palette (indexed) -- one byte/pixel at bit depth 8; needs PLTE, read below
        case 4: bpp = 2; break; // grayscale + alpha
        case 6: bpp = 4; break; // RGBA
        default:
            printf("cover image: PNG color type %u not supported\n", color_type);
            return false;
    }
    // Palette images commonly pack to fewer than 8 bits/pixel (the PNG spec
    // allows 1/2/4/8 for color type 3) when the palette is small -- e.g. a
    // 16-colour palette only needs 4 bits/pixel. Every other supported
    // color type here is 8-bit-only (grayscale/RGB/RGBA at <8 bits/pixel
    // is rare and unsupported).
    bool bit_depth_ok = (bit_depth == 8) ||
                         (color_type == 3 && (bit_depth == 1 || bit_depth == 2 || bit_depth == 4));
    if (!bit_depth_ok) {
        printf("cover image: PNG bit depth %u not supported%s\n", (unsigned)bit_depth,
               color_type == 3 ? " (palette needs 1/2/4/8)" : " (need 8)");
        return false;
    }
    if (interlace != 0) {
        printf("cover image: interlaced (Adam7) PNG not supported\n");
        return false;
    }
    if (width == 0 || width > MAX_PNG_SRC_W || height == 0 || height > MAX_PNG_SRC_H) {
        printf("cover image: PNG %lux%lu outside supported size (max %ux%u)\n",
               (unsigned long)width, (unsigned long)height, MAX_PNG_SRC_W, MAX_PNG_SRC_H);
        return false;
    }

    // Skip any ancillary chunks (gAMA, pHYs, tEXt, ...) between IHDR and the
    // first IDAT; that chunk's [length][type] header primes idat_reader_t.
    // For color type 3, this is also where the required PLTE chunk (always
    // before the first IDAT, per spec) gets captured into s_png_palette.
    idat_reader_t idr = {.fp = fp};
    int palette_entries = 0;
    for (;;) {
        uint8_t ch[8];
        if (f_read(fp, ch, 8, &br) != FR_OK || br != 8) return false;
        uint32_t len = ((uint32_t)ch[0] << 24) | ((uint32_t)ch[1] << 16) | ((uint32_t)ch[2] << 8) | ch[3];
        if (memcmp(ch + 4, "IDAT", 4) == 0) { idr.chunk_remaining = len; break; }
        if (memcmp(ch + 4, "IEND", 4) == 0) return false; // no image data at all
        if (color_type == 3 && memcmp(ch + 4, "PLTE", 4) == 0) {
            if (len == 0 || len > sizeof(s_png_palette) || len % 3 != 0) return false; // malformed
            if (f_read(fp, s_png_palette, len, &br) != FR_OK || br != len) return false;
            palette_entries = (int)(len / 3);
            if (f_lseek(fp, f_tell(fp) + 4) != FR_OK) return false; // skip this chunk's CRC
            continue;
        }
        if (f_lseek(fp, f_tell(fp) + len + 4) != FR_OK) return false; // skip this chunk's data + CRC
    }
    if (color_type == 3 && palette_entries == 0) {
        printf("cover image: palette PNG missing its required PLTE chunk\n");
        return false;
    }

    int extra = fit_decimation((int)width);
    int final_w = (int)width / extra;
    int final_h = (int)height / extra;
    int dst_x0 = (cover_w() - final_w) / 2;
    int dst_y0 = (cover_h() - final_h) / 2;

    // Palette rows are bit-packed (1/2/4/8 bits/pixel, see
    // png_palette_index()'s comment); every other supported color type is
    // bit_depth-8-only, where this is just width*bpp as before.
    size_t row_bytes = color_type == 3 ? (((size_t)width * (size_t)bit_depth + 7) / 8)
                                        : (size_t)width * (size_t)bpp;
    // MUST be static -- up to 4KB each, far past core0's 2KB stack budget.
    static uint8_t prev_row[MAX_PNG_ROW_BYTES];
    static uint8_t cur_row[MAX_PNG_ROW_BYTES];
    memset(prev_row, 0, row_bytes); // first scanline's "previous row" is all-zero per spec

    // static: embeds a 512-byte input buffer, too big for core0's 2KB stack
    // budget alongside everything else already on it by the time this runs
    // (visit_dir() -> cover_image_show() -> here -- see main.c's own notes
    // on core0 stack discipline).
    static png_inflate_t pi;
    png_inflate_init(&pi, &idr);

    for (uint32_t sy = 0; sy < height; sy++) {
        uint8_t filter_type;
        if (!png_inflate_read(&pi, &filter_type, 1)) return false;
        if (!png_inflate_read(&pi, cur_row, row_bytes)) return false;
        png_unfilter(filter_type, cur_row, prev_row, row_bytes, bpp);

        if (sy % (uint32_t)extra == 0) {
            png_blit_row(cur_row, width, bpp, bit_depth, dst_y0 + (int)(sy / (uint32_t)extra), extra, dst_x0,
                         color_type == 3 ? s_png_palette : NULL);
        }
        memcpy(prev_row, cur_row, row_bytes);
    }
    return true;
}

// ============================================================================
// Public API
// ============================================================================

void cover_image_show(const char *path) {
    if (!player_config_display_is_tft()) return; // oled: no room/colour for this, ever

    // Decoding reads the image file from the SD card in many small chunks
    // over what can be a fairly long stretch of wall-clock time -- far
    // longer and far more SD-access-heavy than anything else core0 does
    // while core1's TFT redraw keeps running every redraw_ms (500ms).
    // spi0_bus_lock.h's mutex already serializes each INDIVIDUAL SD
    // transaction against a TFT push, but FatFs issues several separate
    // disk_read() calls per high-level operation with the mutex released
    // in between each one -- gaps a single short VGM-streaming read rarely
    // hits, but a whole image decode's worth of them makes far likelier.
    // Hit in the field (2026-10-02): an SD card that doesn't fully let go
    // of the shared bus between its own transactions (see sd_spi.c's
    // sd_spi_deselect() comment) got left in a bad state by exactly this,
    // and never recovered -- every later directory listing failed with
    // FR_DISK_ERR for the rest of the session, not just the one in
    // progress.
    //
    // First fix tried here was multicore_lockout_start/end_blocking()
    // (parking core1 entirely for the whole decode, same tool flash_disk.c
    // uses for its own flash writes) -- it did NOT help (still reproduced
    // 2026-10-02). Root cause: multicore_lockout's pause is an asynchronous
    // inter-core interrupt that can land on core1 at ANY instruction,
    // including mid-byte inside st7735.c's spi_write_blocking() calls --
    // after spi0_bus_lock() was already taken but before spi0_bus_unlock()
    // runs. That leaves the TFT's CS asserted and the mutex permanently held
    // by a now-parked core1: exactly the bad bus state sd_spi.c's deselect
    // comment warns never recovers on its own. Taking the real
    // spi0_bus_lock() here instead has no such gap -- st7735.c already wraps
    // every one of its own transactions in this SAME mutex, so holding it
    // for the whole decode just makes core1 block cooperatively at its own
    // next lock attempt (between transactions, never mid-transfer) until
    // this decode releases it.
    spi0_bus_lock();

    tft_cover_clear(); // always start from blank -- see this function's own header doc
    if (!path || !path[0]) {
        spi0_bus_unlock();
        return;
    }

    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        printf("cover image: could not open %s\n", path);
        spi0_bus_unlock();
        return;
    }
    bool ok = has_ext(path, ".png") ? decode_png(&file) : decode_jpeg(&file);
    f_close(&file);

    if (!ok) {
        printf("cover image: failed to decode %s, leaving the area blank\n", path);
        tft_cover_clear(); // a failed decode may have blitted a partial image already
    }
    spi0_bus_unlock();
}
