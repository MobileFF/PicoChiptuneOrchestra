// slave_bus.h -- SPI1 master bus that fans out to every slave board over a
// shared SCK/MOSI with one GPIO chip-select per slave (see
// protocol/vgm_spi_protocol.h and docs/circuit.md). Which physical slave
// (if any) handles a given VGM chip is a per-build wiring decision -- edit
// the table at the top of slave_bus.c to match your hardware.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "vgm_chips.h"

void slave_bus_init(void);

// Runtime overrides for the routing table, applied on top of the built-in
// defaults. Call these BEFORE slave_bus_init() (player_config reads them
// from the SD card's vgmplay.ini). An out-of-range chip is ignored; a
// cs_gpio outside 0-28 is ignored with a logged warning.
void slave_bus_set_present(vgm_chip_id_t chip, bool present);
void slave_bus_set_cs_gpio(vgm_chip_id_t chip, unsigned cs_gpio);
void slave_bus_set_gap_us(vgm_chip_id_t chip, uint32_t gap_us);

// True if this build's routing table has a slave wired up for this chip.
bool slave_bus_has_chip(vgm_chip_id_t chip);

void slave_bus_reset(vgm_chip_id_t chip, uint8_t clock_preset);

// Re-assert the clock preset without resetting the chip (VGMSPI_OP_CLOCK).
// Safe to call repeatedly during playback -- the master sends it once per
// song-second so a slave that missed the one-shot RESET at song start
// self-corrects its render rate. See vgm_spi_protocol.h.
void slave_bus_set_clock(vgm_chip_id_t chip, uint8_t clock_preset);
void slave_bus_write(vgm_chip_id_t chip, uint8_t port, uint8_t reg, uint8_t data);
// Low-level send for opcodes other than WRITE0/WRITE1 (currently just the
// SCC-only opcodes -- see protocol/vgm_spi_protocol.h).
void slave_bus_send(vgm_chip_id_t chip, uint8_t opcode, uint8_t reg, uint8_t data);
void slave_bus_mute_all(void);
