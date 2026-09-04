// slave_spi_rx.h
//
// Hardware SPI-slave receiver. Runs on core0 of a slave board: blocks
// forever reading VGM_SPI_FRAME_SIZE-byte frames from the master and
// forwarding each one to core1 (the audio engine, see slave_engine.h) over
// the RP2040's inter-core hardware FIFO.
#pragma once

#include "pico/stdlib.h"
#include "vgm_spi_protocol.h" // VGMSPI_MASK_<CHIP> for the valid_opcode_mask arg

// pin_sck/pin_mosi/pin_cs must be the RX/SCK/CSn-capable GPIOs for the given
// SPI peripheral (spi_index 0 or 1) per the RP2040 GPIO function table -- see
// docs/circuit.md for the concrete pin assignments used by each slave board.
// MISO is intentionally not used (write-only bus); do not wire it.
//
// valid_opcode_mask: bit i set = opcode i is one this slave can legitimately
// receive (use the VGMSPI_MASK_<CHIP> macro from vgm_spi_protocol.h). Used to
// reject stray bytes in the opcode position when resyncing after a dropped
// SPI byte.
//
// Never returns.
void slave_spi_rx_run(uint spi_index, uint pin_sck, uint pin_mosi, uint pin_cs,
                      uint32_t valid_opcode_mask);
