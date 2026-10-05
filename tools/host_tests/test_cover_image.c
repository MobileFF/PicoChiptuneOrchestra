// Host test for cover_image.c's JPEG/PNG decoding -- links the REAL
// cover_image.c + third_party/tjpgd/tjpgd.c + third_party/miniz_tinfl +
// inflate_scratch.c (not a reimplementation) against real test images (see
// gen_cover_test_images.py / cover_test_images/, committed so this test
// needs no image library at run time). Stubs only the two st7735.h
// functions that need real hardware (cover_clear/cover_blit -- captured
// into a plain test framebuffer here instead), player_config_display_is_tft()
// (forced true: this test's whole point is exercising the tft-only decode
// path), and spi0_bus_lock()/spi0_bus_unlock() (no-ops here -- real
// serialization against a concurrent core1 TFT redraw needs no testing on a
// single-threaded host; see spi0_bus_lock.h and cover_image.c's own comment
// on why cover_image_show() holds this mutex for its whole decode). Run from
// tools/host_tests/ (relative paths to cover_test_images/).
//
// Compile (one line):
//   gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../third_party/tjpgd
//       -I ../../third_party/miniz_tinfl -lm
//       test_cover_image.c ../../src/master/src/cover_image.c
//       ../../src/master/src/inflate_scratch.c
//       ../../third_party/tjpgd/tjpgd.c ../../third_party/miniz_tinfl/miniz_tinfl.c
//       -o /tmp/cover_image_test
//   (cd tools/host_tests && /tmp/cover_image_test)
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cover_image.h"
#include "st7735.h"
#include "player_config.h"
#include "spi0_bus_lock.h"

bool player_config_display_is_tft(void) { return true; }
void spi0_bus_lock(void) {}
void spi0_bus_unlock(void) {}

static uint16_t g_fb[COVER_AREA_H][COVER_AREA_W];

void st7735_cover_clear(void) {
    for (int y = 0; y < COVER_AREA_H; y++)
        for (int x = 0; x < COVER_AREA_W; x++)
            g_fb[y][x] = 0x0000;
}

void st7735_cover_blit(int x, int y, int w, int h, const uint16_t *pixels) {
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= COVER_AREA_H) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= COVER_AREA_W) continue;
            g_fb[dy][dx] = pixels[row * w + col];
        }
    }
}

#define FIXDIR "cover_test_images/"

static int fail = 0;
#define CHK(c) do { if (!(c)) { printf("FAIL: %s (%s:%d)\n", #c, __FILE__, __LINE__); fail = 1; } } while (0)

static void rgb565_to_rgb(uint16_t c, int *r, int *g, int *b) {
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5) & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}

// tol should cover ONLY the lossiness actually expected for the check being
// made -- RGB565_TOL is the inherent, bit-exact rounding loss from packing
// 8-bit channels into 5/6/5 bits (e.g. 255 -> 31 -> 248, a diff of 7 on the
// 5-bit channels; up to 3 on the 6-bit green channel), nothing to do with
// decode correctness. JPEG tests add real decode tolerance on top of this.
#define RGB565_TOL 7

static bool close_to(uint16_t c, int er, int eg, int eb, int tol) {
    int r, g, b;
    rgb565_to_rgb(c, &r, &g, &b);
    return abs(r - er) <= tol && abs(g - eg) <= tol && abs(b - eb) <= tol;
}

static bool is_blank(void) {
    for (int y = 0; y < COVER_AREA_H; y++)
        for (int x = 0; x < COVER_AREA_W; x++)
            if (g_fb[y][x] != 0x0000) return false;
    return true;
}

int main(void) {
    // wide_red.png: 300x200 RGB, pure red (255,0,0 -> exact RGB565 0xF800).
    // Wider than COVER_AREA_W(128) -> fit_decimation ceil(300/128) = 3.
    // final_w=300/3=100 final_h=200/3=66, centered: dst_x0=(128-100)/2=14,
    // dst_y0=(96-66)/2=15. Center of the drawn image: (14+50, 15+33).
    cover_image_show(FIXDIR "wide_red.png");
    CHK(close_to(g_fb[48][64], 255, 0, 0, RGB565_TOL));
    CHK(g_fb[0][0] == 0x0000); // letterboxed corner stays background

    // small_blue_rgba.png: 64x64 RGBA, pure blue (alpha ignored). Smaller
    // than the cover area -> extra=1 (no shrink), centered:
    // dst_x0=(128-64)/2=32, dst_y0=(96-64)/2=16.
    cover_image_show(FIXDIR "small_blue_rgba.png");
    CHK(close_to(g_fb[48][64], 0, 0, 255, RGB565_TOL));
    CHK(g_fb[0][0] == 0x0000);
    CHK(g_fb[95][127] == 0x0000); // opposite corner also letterboxed

    // gray.png: 40x40 grayscale, value 0xA8=168 (R/G/B all equal).
    cover_image_show(FIXDIR "gray.png");
    CHK(close_to(g_fb[48][64], 168, 168, 168, RGB565_TOL));

    // wide_green.jpg: 512x256 baseline JPEG, solid green. Lossy, so this
    // only checks "close to green", not exact -- a flat-color image should
    // still decode with very little DCT/quantization error in the interior.
    cover_image_show(FIXDIR "wide_green.jpg");
    CHK(close_to(g_fb[48][64], 0, 200, 0, 30));

    // palette_bicolor.png: color type 3 (palette), 32x32, top half pixel
    // value (index) 0 -> palette black, bottom half index 1 -> palette
    // white. Centered (32 < COVER_AREA_W, no shrink) at dst_x0=48,
    // dst_y0=32, so rows 32-47 are the black half and 48-63 the white half.
    // Checks an actual PLTE lookup happened -- a bug that read the raw
    // index as a gray level instead would turn this into two near-black
    // shades (0/255 -> 0x0000 either way by coincidence for index 0, but
    // index 1 read as gray level 1 would NOT come out as white) rather than
    // true black/white, so the white check is the one that actually proves
    // the palette table is being used, not just index 0 happening to equal
    // black either way.
    cover_image_show(FIXDIR "palette_bicolor.png");
    CHK(g_fb[40][60] == 0x0000);
    CHK(g_fb[55][60] == 0xFFFF);

    // palette_4bit.png: same layout as palette_bicolor.png, but packed at
    // 4 bits/pixel (a small 6-entry palette, indices 0 and 5) instead of a
    // plain byte per pixel -- real-world retro game cover art very often
    // packs this tightly. Hit in the field (2026-10-05): "PNG bit depth 4
    // not supported". Index 5 (not 1) for the white half specifically
    // checks the nibble-extraction math itself (png_palette_index()), not
    // just "is bit_depth 4 accepted at all".
    cover_image_show(FIXDIR "palette_4bit.png");
    CHK(g_fb[40][60] == 0x0000);
    CHK(g_fb[55][60] == 0xFFFF);

    // A path that doesn't exist at all, and path == NULL: both just blank.
    st7735_cover_blit(0, 0, 1, 1, (const uint16_t[]){0xFFFF}); // dirty the fb first
    cover_image_show(FIXDIR "does_not_exist.png");
    CHK(is_blank());

    st7735_cover_blit(0, 0, 1, 1, (const uint16_t[]){0xFFFF});
    cover_image_show(NULL);
    CHK(is_blank());

    // cover_image_is_supported(): the extension check main.c's folder scan uses.
    CHK(cover_image_is_supported("cover.jpg"));
    CHK(cover_image_is_supported("COVER.JPEG"));
    CHK(cover_image_is_supported("folder.PNG"));
    CHK(!cover_image_is_supported("readme.txt"));
    CHK(!cover_image_is_supported("song.vgm"));

    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
