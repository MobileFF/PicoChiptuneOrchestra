#!/usr/bin/env python3
"""Generates the test fixture images committed under cover_test_images/, used
by test_cover_image.c. Needs Pillow (`pip install Pillow`). Not run as part
of the normal test -- the fixtures are small (a few KB total) and committed,
so test_cover_image.c itself needs nothing beyond a C compiler. Re-run this
only if you need to change/add a fixture.
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "cover_test_images")
os.makedirs(OUT, exist_ok=True)


def save(name, img, **kw):
    path = os.path.join(OUT, name)
    img.save(path, **kw)
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


# Wider than COVER_AREA_W (128) -- exercises the width-fit shrink path.
# Pure red so RGB565 round-trips exactly (255,0,0 -> 0xF800), no PNG filter
# surprises since every pixel/row is identical (filter type ends up "None").
save("wide_red.png", Image.new("RGB", (300, 200), (255, 0, 0)))

# Smaller than COVER_AREA_W/H -- exercises "don't upscale, just center"
# (extra=1) and the RGBA/alpha-ignored path.
save("small_blue_rgba.png", Image.new("RGBA", (64, 64), (0, 0, 255, 128)))

# Grayscale (color type 0, bpp 1) -- a mid-gray that round-trips exactly
# through RGB565's 5/6/5 truncation (0xA8 -> R=0xA8>>3<<3=0xA8, G keeps 6
# bits, B truncates the same as R; picked so R and B truncate to the same
# value as the original for an easy exact-match assertion).
save("gray.png", Image.new("L", (40, 40), 0xA8))

# Baseline JPEG, solid green -- lossy, so the test checks "close to green"
# rather than an exact RGB565 value. Also wider than COVER_AREA_W so the
# jpeg_out() extra-decimation path (beyond tjpgd's own power-of-2 scale)
# gets exercised, not just tjpgd's built-in scaling.
save("wide_green.jpg", Image.new("RGB", (512, 256), (0, 200, 0)), quality=90)

# Palette-mode PNG (color type 3) -- top half pixel value (index) 0 ->
# palette black, bottom half index 1 -> palette white. Exercises an actual
# per-pixel PLTE lookup (not just "happens to look right by coincidence"):
# a bug that read the raw index as if it were a gray level, for instance,
# would turn this into two shades of near-black instead of black/white.
w, h = 32, 32
pal_data = bytes([0]) * (w * (h // 2)) + bytes([1]) * (w * (h // 2))
pal_img = Image.frombytes("P", (w, h), pal_data)
pal_img.putpalette([0, 0, 0, 255, 255, 255] + [0] * (256 * 3 - 6))
save("palette_bicolor.png", pal_img)

# Palette-mode PNG, but with a SMALL palette (6 entries) so Pillow's own PNG
# encoder picks 4 bits/pixel instead of 8 -- real-world retro game cover art
# very often does this (hit in the field, 2026-10-05: "PNG bit depth 4 not
# supported"). Index 0 (unused colour, black) for the top half, index 5
# (white, picked to require the full 4 bits -- a bug that only extracted
# the low/high nibble correctly for SOME values, not all, would show up
# here but not necessarily with a 1-bit-pattern index like 1) for the
# bottom half.
pal4_data = bytes([0]) * (w * (h // 2)) + bytes([5]) * (w * (h // 2))
pal4_img = Image.frombytes("P", (w, h), pal4_data)
pal4_img.putpalette([0, 0, 0,  0, 0, 0,  0, 0, 0,  0, 0, 0,  0, 0, 0,  255, 255, 255])
save("palette_4bit.png", pal4_img)
