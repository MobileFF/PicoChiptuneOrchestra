// Sega PCM (315-5218, 16-channel 8-bit PCM sample player) software
// emulator. Unlike every other slave in this repo, this chip needs its
// sample ROM uploaded at runtime (real hardware has it in mask ROM) -- see
// VGMSPI_OP_PCM_UPLOAD_RESET/BYTE in protocol/vgm_spi_protocol.h. The
// upload is capped at SEGAPCM_ROM_BUDGET bytes: real single-song VGM rips
// typically embed tens of KB of samples (20-130KB across several arcade
// game packs surveyed on vgmrips.net), comfortably under the cap, but a
// densely-sampled song could still exceed it -- overflow bytes are
// silently dropped rather than corrupting adjacent memory.
//
// The budget is a *window*, not an absolute 0..192KB range: the master
// sends VGMSPI_OP_SEGAPCM_ROM_BASE (port 9) = the song's lowest sample
// address rounded down to 64KB, and everything here works in
// (absolute_addr - base). So an OutRun-class song whose samples sit at ROM
// 0x40000+ in a 512KB space still plays, as long as the addresses it
// actually touches span less than the budget. See docs/design-notes.md 5.
#pragma once

#include <stdint.h>

#define SEGAPCM_SAMPLE_RATE_HZ 31250u // preset 0: 4000000 / 128 exactly (preset 2 / 8 MHz is 62500)
#define SEGAPCM_ROM_BUDGET (192u * 1024u)

void segapcm_reset(uint8_t clock_preset);
// Clock presets (index-aligned with master/src/vgm_player.c): 0 = 4 MHz
// (System 16 / OutRun era, the common case), 1 = 3.58 MHz (rare safety
// net), 2 = 8 MHz (Sega X-Board / System 18, e.g. After Burner / "02
// Theme.vgm"). Unlike SN76489/AY-3-8910, this chip does NOT resample to a
// fixed output rate (see segapcm_render()'s native clock/128 pacing in
// chip_segapcm.c), so a wrong preset is a uniform pitch/tempo shift
// (preset 2 vs 0 is a full 2x). See chip_ops_t.sample_rate_hz's doc comment.
uint32_t segapcm_sample_rate_hz(uint8_t clock_preset);
// port: 0 = register write (reg = #0x00-0xFF, data = value; see
//       chip_segapcm.c for the layout), 6 = seek upload cursor (reg/data =
//       absolute addr bits [23:16]/[15:8]), 7 = upload next ROM byte (reg
//       ignored, data = byte), 8 = bank config (reg = bankshift, data =
//       bankmask), 9 = ROM-window base (reg/data = base bits [23:16]/[15:8]).
//       Ports 6-9 aren't real chip ports -- see slave_engine.c's
//       VGMSPI_OP_PCM_UPLOAD_*/SEGAPCM_* dispatch and the SCC slave for the
//       same "reuse the generic write() with a distinguishing port" pattern.
void segapcm_write(uint8_t port, uint8_t reg, uint8_t data);
int16_t segapcm_render(void);
