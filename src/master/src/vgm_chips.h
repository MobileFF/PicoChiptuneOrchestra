// vgm_chips.h -- which sound chips this build knows how to dispatch to a
// slave, and the small nearest-match helper used to turn a VGM header's
// actual clock value into one of a slave's compile-time clock presets (see
// VGMSPI_OP_RESET in protocol/vgm_spi_protocol.h).
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum {
    VGM_CHIP_SN76489 = 0,
    VGM_CHIP_YM2413,
    VGM_CHIP_YM2612,
    VGM_CHIP_AY8910,
    VGM_CHIP_YM2151,
    VGM_CHIP_YM2203,
    VGM_CHIP_SCC,
    VGM_CHIP_SEGAPCM,
    // Second SN76489 (VGM spec "Dual Chip Support": header 0x0C clock's bit
    // 30 declares it, command 0x30 dd addresses it instead of 0x50 dd) --
    // appended at the end, not next to VGM_CHIP_SN76489, so every existing
    // chip keeps its numeric value (nothing in this codebase should depend
    // on these being contiguous or ordered by datasheet grouping, but no
    // reason to risk it).
    VGM_CHIP_SN76489_2,
    VGM_CHIP_COUNT,
} vgm_chip_id_t;

// Returns the index into `presets` closest to `actual_hz`. `actual_hz` of 0
// (chip absent from this VGM file) always maps to preset 0.
uint8_t vgm_pick_clock_preset(uint32_t actual_hz, const uint32_t *presets, size_t count);

// Short human-readable name for a chip id ("YM2151", "AY-3-8910", ...), for
// logs. Returns "?" for an out-of-range id.
const char *vgm_chip_name(vgm_chip_id_t chip);
