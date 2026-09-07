// YM2612 (Sega Mega Drive/Genesis FM) emulator: thin C wrapper around the
// vendored ymfm library (third_party/ymfm). Native output rate is derived
// from the chip's clock (input_clock / 144); ym2612_reset() itself doesn't
// need to know the clock (see chip_ym2612.cpp), but ym2612_sample_rate_hz()
// does, to pick the correct render rate for the NTSC/PAL preset.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// clock/144 for the default (NTSC, 7670454 Hz) clock preset. Measured via
// ymfm's ym2612::sample_rate() in tools/probes/ym2612_probe.cpp -- see
// ym2612_sample_rate_hz() for the PAL preset actually used at runtime.
#define YM2612_SAMPLE_RATE_HZ 53267u

// Cap on the uploaded Mega Drive PCM data bank (VGM 0x67 type-0 blocks).
// Most single-song MD rips embed well under this; a bigger voice-heavy bank
// has its tail dropped (reads past the end return DAC-centre 0x80). Tunable
// via CMake -DVGM_YM2612_PCM_BUDGET=N.
#ifndef YM2612_PCM_BUDGET
#define YM2612_PCM_BUDGET (192u * 1024u)
#endif

void ym2612_reset(uint8_t clock_preset);
// port 0 / 1 = the chip's two real register banks (VGMSPI_OP_WRITE0/WRITE1).
// Ports 20-25 carry the YM2612 PCM/DAC-streaming opcodes (not real chip
// ports -- see slave_engine.c and VGMSPI_OP_YM2612_* in
// protocol/vgm_spi_protocol.h): 20 = PCM bank upload-cursor reset, 21 =
// upload one bank byte (data), 22 = DAC_START (reg:data = read offset
// [15:0]), 23 = DAC_RATE (reg:data = bytes-per-sample * 65536), 24 =
// DAC_STOP, 25 = DAC_SEEK (data = read offset [23:16], latched for the next
// DAC_START).
void ym2612_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t ym2612_render(void);
// NTSC (preset 0, 7670454 Hz) vs PAL (preset 1, 7600489 Hz) Genesis --
// about 1% apart, the same class of approximation already accepted for
// SN76489/AY-3-8910. Unlike those chips, this one does NOT resample
// internally, so getting the preset wrong shows up as a uniform pitch/tempo
// shift across the whole song rather than a crash or glitch. See
// chip_ops_t.sample_rate_hz's doc comment.
uint32_t ym2612_sample_rate_hz(uint8_t clock_preset);

#ifdef __cplusplus
}
#endif
