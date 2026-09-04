// vgm_spi_protocol.h
//
// Shared wire protocol between the master (Pico, VGM parser/dispatcher) and
// each slave (RP2040-Zero, one sound chip emulator). Included by both sides.
//
// Bus shape: one shared SCK + MOSI (write-only, MISO not wired), one GPIO
// chip-select per slave. A "frame" is logically 3 bytes, but the master
// sends each byte as its own separate CS-low/write/CS-high pulse (~20us gap
// between pulses) rather than holding CS low across all 3 -- on the
// RP2040-to-RP2040 hardware this was built/tested against, holding CS low
// for a continuous multi-byte burst reliably dropped bytes 2-3 with no
// error indication (see master/src/slave_bus.c send_frame(),
// tools/spi_bringup_test/, docs/design-notes.md). The 20us gap was found by
// real-hardware testing: 50us was reliable but added enough per-frame
// latency to audibly perturb playback tempo in write-dense passages; 5us
// was too short and byte loss reappeared; 20us was reliable and fixed the
// tempo issue. The slave's hardware SPI peripheral only clocks in data
// while its own CS is asserted, so many slaves can still share SCK/MOSI
// safely (standard SPI multi-drop). See docs/circuit.md.
//
// The protocol is intentionally chip-agnostic: the master only knows "which
// VGM chip command maps to which slave + which opcode". Each slave interprets
// reg/data according to the one chip it emulates.
#pragma once

#include <stdint.h>

#define VGM_SPI_FRAME_SIZE 3
#define VGM_SPI_BAUD_HZ (4 * 1000 * 1000) // 4 MHz; plenty of headroom, short traces

typedef struct __attribute__((packed)) {
    uint8_t opcode;
    uint8_t reg;  // register/address; meaning is opcode+chip specific
    uint8_t data; // data byte; meaning is opcode+chip specific
} vgm_spi_frame_t;

enum vgm_spi_opcode {
    // No-op. Slaves ignore reg/data. Used by the master to warm up / probe a
    // bus without side effects.
    VGMSPI_OP_NOP = 0x00,

    // Reset the emulated chip to its power-on state.
    //   reg  = clock preset index (chip-specific table, 0 = chip's most
    //          common clock, e.g. NTSC). Lets the master tell a slave which
    //          of a handful of real-world clock rates a given VGM file was
    //          authored for, without growing the frame beyond 3 bytes.
    //   data = ignored.
    VGMSPI_OP_RESET = 0x01,

    // Write to the chip's primary register port.
    //   SN76489  : reg unused (0), data = raw SN76489 command byte
    //              (mirrors VGM command 0x50 dd)
    //   AY-3-8910: reg = register #0-15,        data = value  (VGM 0xA0 aa dd)
    //   YM2413   : reg = register #,             data = value  (VGM 0x51 aa dd)
    //   YM2612   : reg = register #, port 0,     data = value  (VGM 0x52 aa dd)
    //   YM2151   : reg = register #,             data = value  (VGM 0x54 aa dd)
    //   YM2203   : reg = register # (FM or SSG), data = value  (VGM 0x55 aa dd)
    VGMSPI_OP_WRITE0 = 0x02,

    // Write to the chip's secondary register port (chips that have one).
    //   YM2612   : reg = register #, port 1,     data = value  (VGM 0x53 aa dd)
    // Slaves for chips without a second port ignore this opcode.
    VGMSPI_OP_WRITE1 = 0x03,

    // Mute / silence the chip immediately (used on stop, seek, EOF).
    VGMSPI_OP_MUTE = 0x04,

    // K051649/SCC (Konami SCC) only, below. SCC's real hardware protocol is
    // a 2-step "latch a sub-address, then write" scheme across 5 regions
    // (waveform/frequency/volume/keyon/test); rather than replicate that
    // 2-cycle handshake over the wire, the master pre-decodes which region a
    // VGM 0xD2 command targets and sends the matching opcode directly below
    // -- functionally identical, one frame instead of two. See
    // slave_scc/src/chip_scc.c and docs/design-notes.md.
    //   reg = waveform table offset 0x00-0x7F (0x60-0x7F shared by ch3/ch4,
    //         matching real hardware), data = signed 8-bit waveform sample.
    VGMSPI_OP_SCC_WAVEFORM = 0x05,
    //   reg = channel*2 + (0=low byte, 1=high nibble), data = frequency byte.
    VGMSPI_OP_SCC_FREQ = 0x06,
    //   reg = channel # (0-4), data = volume 0-15 (low nibble).
    VGMSPI_OP_SCC_VOLUME = 0x07,
    //   reg unused, data = 5-bit key on/off mask, one bit per channel.
    VGMSPI_OP_SCC_KEYON = 0x08,

    // Sega PCM only, below. Sega PCM's sample ROM is uploaded from a VGM
    // data block (a single one-time bulk transfer per song, not a
    // steady-state control-rate write like everything else in this
    // protocol). Rather than design a separate variable-length bulk-SPI
    // mode, the master just streams it as one VGMSPI_OP_PCM_BYTE frame per
    // byte -- simple, but slow: each frame costs ~65us (dominated by the
    // 3 CS pulses' inter-pulse gaps, see the per-byte-CS-pulse note above),
    // so a full 192KB upload (the slave's capped buffer, see
    // slave_segapcm/src/chip_segapcm.h) takes on the order of 12-13 seconds.
    // Expect a real pause at the start of a Sega PCM song while the ROM
    // uploads. See slave_segapcm/src/chip_segapcm.c and docs/design-notes.md.
    //   Seeks the slave's upload cursor. reg/data = bits [23:16]/[15:8] of
    //   the target ROM address, i.e. the seek is 256-byte granular (no room
    //   for addr[7:0] in a 3-byte frame). reg=data=0 is the plain "rewind to
    //   0" sent once at song start. The master also sends one of these
    //   before each VGM 0x67 type-0x80 ROM-image block's bytes, carrying
    //   that block's VGM-declared start address (256-aligned down), because a
    //   song's ROM may arrive as several chunks at different addresses (see
    //   master/src/vgm_player.c handle_data_block()). Chunk starts are NOT
    //   always 256-aligned (tightly-packed rips are byte-granular), so the
    //   master follows the seek with (start_addr & 0xFF) UPLOAD_BYTE frames
    //   of 0x80 (silence) to line the real data up to the exact byte.
    //   Addresses are absolute (as declared in the VGM block); the slave
    //   subtracts whatever VGMSPI_OP_SEGAPCM_ROM_BASE it was given.
    VGMSPI_OP_PCM_UPLOAD_RESET = 0x09,
    //   reg ignored, data = next ROM byte. Auto-increments the cursor;
    //   bytes at/after the slave's fixed ROM budget are silently dropped
    //   (so is any write while the cursor was seeked past the budget). The
    //   8-byte ROM-image-block prefix (total size + start address) is
    //   stripped by the master and never sent as an upload byte.
    VGMSPI_OP_PCM_UPLOAD_BYTE = 0x0A,

    // Sega PCM bank addressing config, derived by the master from the VGM
    // header's "Sega PCM interface register" (offset 0x3C) the same way
    // libvgm/MAME do. Sent once per song, before the ROM upload. Each
    // channel's CTRL register (0x86 + ch*8) carries a bank number in its
    // upper bits; the ROM byte a channel reads is
    //   rom[((ctrl & bankmask) << bankshift) | (addr16)]
    //   reg  = bankshift (0 -> slave default 12)
    //   data = bankmask  (0 -> slave default 0x70)
    VGMSPI_OP_SEGAPCM_BANK = 0x0B,
    // Register writes use the ordinary VGMSPI_OP_WRITE0 (reg = register
    // #0x00-0x7F within the 16-channel x 8-byte register bank, data =
    // value -- matches VGM 0xC0 bbaa dd with the bank-select bits of `bbaa`
    // masked off, see master/src/vgm_player.c).

    // Sega PCM ROM-window base address. Sent once per song, right before the
    // song-start UPLOAD_RESET. A song's sample ROM can sit high in a large
    // (e.g. 512KB OutRun) address space while spanning far less than the
    // slave's fixed ROM budget -- so the master pre-scans the song's 0x67
    // type-0x80 blocks, takes the lowest start address rounded down to 64KB
    // (the coarsest Sega PCM bank granularity), and sends it here. The slave
    // then stores/reads sample ROM at (absolute_addr - base), making
    // OutRun-class songs fit the budget without more RAM. base 0 (the
    // default, and what non-Sega-PCM or low-ROM songs get) = no rebasing.
    //   reg  = base[23:16], data = base[15:8]  (low 8 bits always 0)
    VGMSPI_OP_SEGAPCM_ROM_BASE = 0x0C,

    // Re-assert the clock preset WITHOUT resetting the chip. Same reg as
    // VGMSPI_OP_RESET (clock preset index); data ignored. The slave only
    // updates its render-loop pacing (sample_rate_hz) and leaves every
    // register untouched -- so the master can send this repeatedly during
    // playback. Fixes: RESET is a one-shot at song start, and a slave that
    // isn't listening yet at that instant (still booting, or reflashed
    // mid-session) otherwise stays at its power-on default rate for the
    // whole song (heard as a uniform pitch/tempo shift -- YM2151 at 55930
    // instead of 62500 Hz = ~2 semitones flat). The master re-sends this
    // once per second of song time; a late slave self-corrects on the next
    // one. A no-op for SN76489/AY-3-8910/SCC (they resample to a fixed
    // output rate, so sample_rate_hz never changes).
    VGMSPI_OP_CLOCK = 0x0D,

    // Not a real opcode: one past the highest valid one. slave_spi_rx.c
    // range-checks the byte in a frame's opcode position against this while
    // resyncing after a dropped byte -- keep it last in this enum.
    VGMSPI_OP__COUNT
};

// Per-slave-type set of opcodes the master will ever send that chip, as a
// bitmask indexed by opcode value. slave_spi_rx.c passes the right one to
// slave_spi_rx_run() and uses it to reject a byte in the opcode position
// while resyncing after a dropped SPI byte -- tighter than the old
// "< VGMSPI_OP__COUNT" range check. Concretely: YM2151 never receives 0x08,
// so a {WRITE0, reg=0x08, data} key-on frame that lost its opcode byte is no
// longer mis-accepted as VGMSPI_OP_SCC_KEYON (=0x08) and consumed as a fake
// 3-byte frame -- the parser discards just the stray byte and keeps hunting.
#define VGMSPI_MASK_BASE \
    ((1u << VGMSPI_OP_NOP) | (1u << VGMSPI_OP_RESET) | (1u << VGMSPI_OP_MUTE) | \
     (1u << VGMSPI_OP_CLOCK))
#define VGMSPI_MASK_SN76489 (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0))
#define VGMSPI_MASK_AY8910  (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0))
#define VGMSPI_MASK_YM2413  (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0))
#define VGMSPI_MASK_YM2151  (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0))
#define VGMSPI_MASK_YM2203  (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0))
#define VGMSPI_MASK_YM2612  (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0) | (1u << VGMSPI_OP_WRITE1))
#define VGMSPI_MASK_SCC     (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_SCC_WAVEFORM) | \
                             (1u << VGMSPI_OP_SCC_FREQ) | (1u << VGMSPI_OP_SCC_VOLUME) | \
                             (1u << VGMSPI_OP_SCC_KEYON))
#define VGMSPI_MASK_SEGAPCM (VGMSPI_MASK_BASE | (1u << VGMSPI_OP_WRITE0) | \
                             (1u << VGMSPI_OP_PCM_UPLOAD_RESET) | (1u << VGMSPI_OP_PCM_UPLOAD_BYTE) | \
                             (1u << VGMSPI_OP_SEGAPCM_BANK) | (1u << VGMSPI_OP_SEGAPCM_ROM_BASE))

// One GPIO shared by all slaves on both SPI buses, driven low->high by the
// master once per output sample tick (44100 Hz). Purely optional: slaves use
// it only to keep long-run phase drift bounded; it is NOT required for basic
// operation. See docs/circuit.md section "clock sync (optional)".
#define VGM_SYNC_GPIO_PURPOSE "44.1kHz sample-tick reference, optional"
