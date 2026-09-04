#include "vgm_chips.h"
#include <stdlib.h>

uint8_t vgm_pick_clock_preset(uint32_t actual_hz, const uint32_t *presets, size_t count) {
    if (actual_hz == 0) return 0;
    size_t best = 0;
    uint32_t best_diff = (actual_hz > presets[0]) ? (actual_hz - presets[0]) : (presets[0] - actual_hz);
    for (size_t i = 1; i < count; i++) {
        uint32_t diff = (actual_hz > presets[i]) ? (actual_hz - presets[i]) : (presets[i] - actual_hz);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return (uint8_t)best;
}

const char *vgm_chip_name(vgm_chip_id_t chip) {
    switch (chip) {
        case VGM_CHIP_SN76489: return "SN76489";
        case VGM_CHIP_YM2413:  return "YM2413";
        case VGM_CHIP_YM2612:  return "YM2612";
        case VGM_CHIP_AY8910:  return "AY-3-8910";
        case VGM_CHIP_YM2151:  return "YM2151";
        case VGM_CHIP_YM2203:  return "YM2203";
        case VGM_CHIP_SCC:     return "SCC";
        case VGM_CHIP_SEGAPCM: return "SegaPCM";
        default:               return "?";
    }
}
