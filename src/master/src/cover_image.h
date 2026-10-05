// cover_image.h -- shows the first image (by filename) found directly in a
// folder, scaled to fit the TFT's cover-art area width (st7735.h's
// COVER_AREA_W/H), once per folder visit -- see main.c's visit_dir(). A
// no-op entirely when [player] display != tft (the OLED has neither the
// resolution nor the colour depth for this).
//
// Supports baseline JPEG (via third_party/tjpgd, ChaN's TJpgDec) and a
// deliberately limited subset of PNG (8-bit depth, non-interlaced,
// grayscale/RGB/RGBA -- no palette, no 16-bit, no Adam7 -- see cover_image.c
// for why; covers the large majority of real-world "cover art" exports).
// Scaling is nearest-neighbour, uniform on both axes (never distorts the
// image), fit to COVER_AREA_W and centred; a source image taller than
// COVER_AREA_H after that width-fit is centred and silently clipped
// top/bottom rather than shrunk further (matching "fit to the display
// width" as asked for, not "fit to the whole box").
#pragma once

#include <stdbool.h>

// True if `name`'s extension is one cover_image_show() can decode
// (.jpg/.jpeg/.png, case-insensitive). main.c's existing per-folder file
// scan uses this the same way it already picks out .vgm/.vgz.
bool cover_image_is_supported(const char *name);

// Decodes `path` (a FatFs path, e.g. "0:/Game/cover.jpg") and draws it into
// the cover area. `path` NULL (or empty) means "no image this folder" --
// just clears the area. A decode failure (corrupt file, unsupported PNG
// variant, truncated stream, ...) is logged and falls back to a cleared
// area, same as NULL, rather than a half-drawn image. Safe to call every
// time regardless of [player] display; it's a silent no-op for oled.
void cover_image_show(const char *path);
