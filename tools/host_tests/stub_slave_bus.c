// Logging stand-in for master/src/slave_bus.c, so vgm_player.c's real
// dispatch logic can be exercised on the host without real SPI hardware.
#include "slave_bus.h"
#include <stdio.h>

static const char *chip_name(vgm_chip_id_t c) {
    switch (c) {
        case VGM_CHIP_SN76489: return "SN76489";
        case VGM_CHIP_YM2413: return "YM2413";
        case VGM_CHIP_YM2612: return "YM2612";
        case VGM_CHIP_AY8910: return "AY8910";
        case VGM_CHIP_YM2151: return "YM2151";
        case VGM_CHIP_YM2203: return "YM2203";
        case VGM_CHIP_SCC: return "SCC";
        case VGM_CHIP_SEGAPCM: return "SEGAPCM";
        default: return "?";
    }
}

void slave_bus_init(void) {}
bool slave_bus_has_chip(vgm_chip_id_t chip) { (void)chip; return true; }

void slave_bus_reset(vgm_chip_id_t chip, uint8_t clock_preset) {
    printf("RESET  %-8s preset=%u\n", chip_name(chip), clock_preset);
}

void slave_bus_set_clock(vgm_chip_id_t chip, uint8_t clock_preset) {
    printf("CLOCK  %-8s preset=%u\n", chip_name(chip), clock_preset);
}

void slave_bus_write(vgm_chip_id_t chip, uint8_t port, uint8_t reg, uint8_t data) {
    printf("WRITE  %-8s port=%u reg=0x%02X data=0x%02X\n", chip_name(chip), port, reg, data);
}

void slave_bus_send(vgm_chip_id_t chip, uint8_t opcode, uint8_t reg, uint8_t data) {
    printf("SEND   %-8s opcode=0x%02X reg=0x%02X data=0x%02X\n", chip_name(chip), opcode, reg, data);
}

void slave_bus_mute_all(void) {
    printf("MUTE all\n");
}
