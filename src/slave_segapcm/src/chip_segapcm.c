#include "chip_segapcm.h"
#include <string.h>

// Register RAM layout (mirrors real hardware exactly, verified against
// MAME's segapcm.c core): 16 channels x 8-byte "low" block starting at
// 0x00 (volume/loop/end/delta), plus a second 8-byte "high" block per
// channel starting at 0x84 (running address + control). Both blocks for
// all 16 channels fit inside a single uint8_t register index (max index:
// 8*15 + 0x87 = 0xFF), so the wire protocol's plain 8-bit `reg` field
// addresses the whole thing with no extra bank/offset bytes needed.
#define REG_VOL_L(ch)    (8 * (ch) + 2)
#define REG_VOL_R(ch)    (8 * (ch) + 3)
#define REG_LOOP_MID(ch) (8 * (ch) + 4)
#define REG_LOOP_HI(ch)  (8 * (ch) + 5)
#define REG_END(ch)      (8 * (ch) + 6)
#define REG_DELTA(ch)    (8 * (ch) + 7)
#define REG_ADDR_MID(ch) (0x84 + 8 * (ch))
#define REG_ADDR_HI(ch)  (0x85 + 8 * (ch))
#define REG_CTRL(ch)     (0x86 + 8 * (ch))
// bit0 of REG_CTRL: 1 = channel disabled. bit1: 1 = disable looping
// (self-disable at end-of-sample instead of jumping to the loop point).

static uint8_t s_rom[SEGAPCM_ROM_BUDGET];
static uint32_t s_upload_cursor;
// ROM-window base (VGMSPI_OP_SEGAPCM_ROM_BASE, port 9). A song whose sample
// ROM sits high in a large address space (e.g. OutRun's 512KB ROM) but
// spans less than SEGAPCM_ROM_BUDGET is stored/read at (absolute - base) so
// it still fits s_rom[]. 0 = no rebasing (non-Sega-PCM or low-ROM songs).
// 64KB-granular, so it never splits a Sega PCM bank.
static uint32_t s_rom_base;

static uint8_t s_regs[256];
static uint8_t s_addr_lo[16]; // fractional/low byte of each channel's running address (chip-internal, not in s_regs -- matches real hardware)

// Bank addressing (VGM header 0x3C interface register, forwarded by the
// master as VGMSPI_OP_SEGAPCM_BANK -> port 8). A channel's CTRL register
// (REG_CTRL) carries a bank number in bits masked by s_bankmask; the ROM
// address is ((ctrl & s_bankmask) << s_bankshift) | (16-bit sample addr).
// Defaults match libvgm/MAME's BANK_512 / BANK_MASK7.
static uint8_t s_bankshift = 12;
static uint8_t s_bankmask = 0x70;

// Must stay index-aligned with master/src/vgm_player.c's own copy. Preset 0
// (4 MHz, System 16/OutRun-era) is the common case; preset 1 (3.58 MHz) is
// a rare safety net; preset 2 (8 MHz) is the Sega X-Board / System 18 rate
// -- twice as fast, so a wrong preset here halves pitch AND tempo. See the
// NOTE on SEGAPCM_CLOCK_PRESETS in master/src/vgm_player.c.
static const uint32_t CLOCK_PRESETS[3] = {4000000, 3579545, 8000000};

void segapcm_reset(uint8_t clock_preset) {
    (void)clock_preset; // register state below doesn't depend on clock; see segapcm_sample_rate_hz()
    // Real hardware's register RAM powers up as 0xFF, which conveniently
    // means every channel's disable bit (REG_CTRL bit0) starts set.
    memset(s_regs, 0xFF, sizeof(s_regs));
    memset(s_addr_lo, 0, sizeof(s_addr_lo));
    // Back to defaults; the master re-sends the real bank config (port 8)
    // right after this on every song that has a Sega PCM clock.
    s_bankshift = 12;
    s_bankmask = 0x70;
    s_rom_base = 0; // master re-sends the real base (port 9) before the upload
    // Clear the sample ROM to digital silence (0x80). A song only uploads the
    // ROM regions IT uses, and consecutive songs from the same board upload
    // DIFFERENT subsets (e.g. OutRun's "Magical Sound Shower" has 12 blocks,
    // "Splash Wave" only 10, at partly different addresses). Without this
    // clear, a channel in song N whose address/loop range strays into a
    // region song N never uploaded reads leftover sample bytes from song
    // N-1 / N-2 -- audible as distortion that gets worse the more SegaPCM
    // songs have played. ~1.5 ms once per song, during the inter-song pause.
    memset(s_rom, 0x80, sizeof(s_rom));
}

uint32_t segapcm_sample_rate_hz(uint8_t clock_preset) {
    uint32_t clock = CLOCK_PRESETS[clock_preset < 3 ? clock_preset : 0];
    return clock / 128;
}

// port 6 (VGMSPI_OP_PCM_UPLOAD_RESET): seek the upload cursor. reg/data are
// bits [23:16]/[15:8] of the target ROM address -- 256-byte granular, which
// is all Sega PCM ROM data blocks ever need (sample/loop addresses are
// themselves 256-aligned). reg=data=0 is the plain "rewind to 0" the master
// sends once per song.
static void upload_seek(uint8_t addr_hi, uint8_t addr_mid) {
    // Addresses on the wire are absolute (VGM-declared); store relative to
    // the song's ROM-window base. A target below the base (e.g. the
    // song-start rewind to 0 on a rebased song) parks the cursor past the
    // budget so the following writes no-op.
    uint32_t absolute = ((uint32_t)addr_hi << 16) | ((uint32_t)addr_mid << 8);
    s_upload_cursor = (absolute >= s_rom_base) ? (absolute - s_rom_base)
                                              : SEGAPCM_ROM_BUDGET;
}

static void upload_byte(uint8_t data) {
    if (s_upload_cursor < SEGAPCM_ROM_BUDGET) {
        s_rom[s_upload_cursor++] = data;
    }
    // else: silently drop -- see SEGAPCM_ROM_BUDGET's doc comment. A seek
    // past the budget followed by writes therefore just no-ops.
}

void segapcm_write(uint8_t port, uint8_t reg, uint8_t data) {
    switch (port) {
        case 0: s_regs[reg] = data; break;
        case 6: upload_seek(reg, data); break;
        case 7: upload_byte(data); break;
        case 8: // VGMSPI_OP_SEGAPCM_BANK: reg = bankshift, data = bankmask (0 = keep default)
            if (reg) s_bankshift = reg;
            if (data) s_bankmask = data;
            break;
        case 9: // VGMSPI_OP_SEGAPCM_ROM_BASE: reg = base[23:16], data = base[15:8]
            s_rom_base = ((uint32_t)reg << 16) | ((uint32_t)data << 8);
            break;
        default: break;
    }
}

int16_t segapcm_render(void) {
    int32_t mix_l = 0, mix_r = 0;

    for (int ch = 0; ch < 16; ch++) {
        uint8_t ctrl = s_regs[REG_CTRL(ch)];
        if (ctrl & 1) continue; // channel disabled

        uint32_t addr = (uint32_t)s_addr_lo[ch]
                       | ((uint32_t)s_regs[REG_ADDR_MID(ch)] << 8)
                       | ((uint32_t)s_regs[REG_ADDR_HI(ch)] << 16);
        uint32_t loop = ((uint32_t)s_regs[REG_LOOP_HI(ch)] << 16)
                       | ((uint32_t)s_regs[REG_LOOP_MID(ch)] << 8);
        uint32_t end = (uint32_t)s_regs[REG_END(ch)] + 1;

        if ((addr >> 16) == end) {
            if (ctrl & 2) {
                s_regs[REG_CTRL(ch)] = ctrl | 1; // self-disable, no output this sample
                continue;
            }
            addr = loop;
        }

        // Real Sega PCM ROM address = bank base (from CTRL's upper bits,
        // per the VGM interface register) | 16-bit sample address. The
        // 0x67 ROM-image blocks are uploaded at their absolute ROM
        // addresses (master strips the 8-byte prefix and seeks), so this
        // reconstructs the same layout. Sample ROM is unsigned 8-bit PCM
        // (0x80 = silence). s_rom[] only covers SEGAPCM_ROM_BUDGET bytes
        // starting at s_rom_base (the song's rebased ROM window, port 9), so
        // anything outside [base, base+budget) reads as silence rather than
        // a wrong sample or an OOB access.
        uint32_t rom_idx = ((uint32_t)(ctrl & s_bankmask) << s_bankshift)
                         | ((addr >> 8) & 0xFFFFu);
        uint32_t rom_rel = rom_idx - s_rom_base;
        int32_t v = (rom_idx >= s_rom_base && rom_rel < SEGAPCM_ROM_BUDGET)
                  ? ((int32_t)s_rom[rom_rel] - 0x80) : 0;

        mix_l += v * (s_regs[REG_VOL_L(ch)] & 0x7F);
        mix_r += v * (s_regs[REG_VOL_R(ch)] & 0x7F);

        addr = (addr + s_regs[REG_DELTA(ch)]) & 0xFFFFFF;
        s_regs[REG_ADDR_MID(ch)] = (uint8_t)(addr >> 8);
        s_regs[REG_ADDR_HI(ch)] = (uint8_t)(addr >> 16);
        s_addr_lo[ch] = (uint8_t)addr;
    }

    // v (+-128) * vol (0-0x7F) per side, summed over active channels. Real
    // Sega PCM songs use small volume-register values (e.g. "02 Theme"
    // maxes at 48/127) and few simultaneous voices, so >>6 came out ~20-50x
    // quieter than the FM slaves on the shared analog mix bus. >>4 (measured
    // peak ~2148, RMS ~324 for "02 Theme" -- see tools/host_tests notes)
    // matches YM2203's own headroom choice; the clamp below trims only the
    // rare loudest transient, as on the FM chips. Drop to >>5 if a
    // full-volume song ever clips audibly.
    int32_t mono = (mix_l + mix_r) >> 4;
    if (mono > 2047) mono = 2047;
    if (mono < -2048) mono = -2048;
    return (int16_t)mono;
}
