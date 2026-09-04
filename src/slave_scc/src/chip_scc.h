// K051649/SCC ("Konami SCC", MSX cartridge wavetable chip) software
// emulator: 5 channels, each reading its own 32-byte user-defined 8-bit
// signed waveform through a frequency-controlled phase accumulator.
// Channels 3/4 (0-indexed) share one waveform table on real hardware
// (classic SCC mode; the newer SCC+/K052539 non-shared mode is not
// implemented -- see docs/design-notes.md).
#pragma once

#include <stdint.h>

#define SCC_SAMPLE_RATE_HZ 44100

void scc_reset(uint8_t clock_preset);
// port: 2=waveform, 3=frequency, 4=volume, 5=keyon (see slave_engine.c and
// protocol/vgm_spi_protocol.h's VGMSPI_OP_SCC_* opcodes).
void scc_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t scc_render(void);
// Always SCC_SAMPLE_RATE_HZ: see sn76489_sample_rate_hz's doc comment, same
// reasoning (this chip resamples internally too).
uint32_t scc_sample_rate_hz(uint8_t clock_preset);
