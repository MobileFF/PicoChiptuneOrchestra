#include "tft_panel.h"

#include "st7735.h"
#include "tft_big.h"

static const tft_geom_t GEOM[] = {
    [PLAYER_TFT_ST7735]  = {ST7735_W, ST7735_H, COVER_AREA_H},
    [PLAYER_TFT_ILI9341] = {240, 320, 160},
    [PLAYER_TFT_ST7796]  = {320, 480, 240},
};

const tft_geom_t *tft_panel_geom(void) { return &GEOM[player_config_tft_panel()]; }

void tft_cover_clear(void) {
    if (player_config_tft_panel() == PLAYER_TFT_ST7735) st7735_cover_clear();
    else tft_big_cover_clear();
}

void tft_cover_blit(int x, int y, int w, int h, const uint16_t *pixels) {
    if (player_config_tft_panel() == PLAYER_TFT_ST7735) st7735_cover_blit(x, y, w, h, pixels);
    else tft_big_cover_blit(x, y, w, h, pixels);
}
