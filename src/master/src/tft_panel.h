// tft_panel.h -- the TFT panel selected by [player] tft_panel (see
// player_config.h): its pixel size and the cover-art area that cover_image.c
// draws into. Dispatches to st7735.c (128x160, RAM framebuffer) or tft_big.c
// (ILI9341 / ST7796, no full-screen framebuffer -- see tft_big.h for why).
#pragma once

#include <stdint.h>
#include "player_config.h"

// Largest panel width this build supports -- sizes the static scratch rows
// shared by cover_image.c and the big-panel driver.
#define TFT_MAX_W 320

typedef struct {
    uint16_t w;        // panel width in pixels (portrait)
    uint16_t h;        // panel height in pixels
    uint16_t cover_h;  // rows at the top reserved for the cover-art image; the
                       // text area is everything below it
} tft_geom_t;

const tft_geom_t *tft_panel_geom(void);

// Draw into the cover-art area (rows 0..cover_h-1). See st7735.h's own
// st7735_cover_clear()/st7735_cover_blit() comments for the contract -- these
// are the same, for whichever panel is selected.
void tft_cover_clear(void);
void tft_cover_blit(int x, int y, int w, int h, const uint16_t *pixels);
