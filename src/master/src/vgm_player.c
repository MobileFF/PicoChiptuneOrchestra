#include "vgm_player.h"

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "pico/stdlib.h"

#include "slave_bus.h"
#include "vgm_chips.h"
#include "vgm_spi_protocol.h"

#define VGM_SAMPLE_HZ 44100

// Timing-diagnosis logs ([FIRST]/[EV]/[LAG]); see the CMake option of the same
// name. Off unless the build turns it on. ([TIME] is separate, PICO_ON_DEVICE.)
#ifndef VGM_MASTER_DEBUG_TRACE
#define VGM_MASTER_DEBUG_TRACE 0
#endif

// Must match the corresponding slave firmware's CLOCK_PRESETS tables
// (slave_sn76489/src/chip_sn76489.c, slave_ay8910/src/chip_ay8910.c,
// slave_ym2612/src/chip_ym2612.cpp, slave_ym2413/src/chip_ym2413.cpp,
// slave_ym2151/src/chip_ym2151.cpp, slave_ym2203/src/chip_ym2203.cpp,
// slave_segapcm/src/chip_segapcm.c).
static const uint32_t SN76489_CLOCK_PRESETS[] = {3579545, 3546893, 4000000};
static const uint32_t AY8910_CLOCK_PRESETS[] = {1789773, 1773400, 2000000};
static const uint32_t YM2612_CLOCK_PRESETS[] = {7670454, 7600489};
static const uint32_t YM2413_CLOCK_PRESETS[] = {3579545, 3546893};
static const uint32_t YM2151_CLOCK_PRESETS[] = {3579545, 4000000};
static const uint32_t YM2203_CLOCK_PRESETS[] = {3993600, 4000000};
// K051649/SCC: 3579545 = the MSX bus clock (real-SCC period values, the
// common case); 1789772 = the half-clock some rips write in the header.
// Index-aligned with CLOCK_PRESETS in slave_scc/src/chip_scc.c.
static const uint32_t SCC_CLOCK_PRESETS[] = {3579545, 1789772};
// idx 2 = 8 MHz (Sega X-Board / System 18, e.g. "02 Theme.vgm"): the slave
// then renders at 62500 Hz instead of 31250. A first attempt at this
// coincided with silent Sega PCM output that later looked like a wiring
// fault, so it's back in; if the output goes silent again, check a
// VGM_SLAVE_VERBOSE_LOG RATE CHECK before assuming the rate is the cause.
static const uint32_t SEGAPCM_CLOCK_PRESETS[] = {4000000, 3579545, 8000000};

// --- buffered, seekable file reader -----------------------------------

typedef struct {
    FIL *fp;
    uint8_t buf[512];
    uint16_t pos, len;
    FSIZE_t buf_file_offset;
} reader_t;

static bool reader_fill(reader_t *r) {
    UINT br = 0;
    r->buf_file_offset = f_tell(r->fp);
    FRESULT fr = f_read(r->fp, r->buf, sizeof(r->buf), &br);
    r->pos = 0;
    r->len = (uint16_t)br;
    return fr == FR_OK && br > 0;
}

static int reader_byte(reader_t *r) {
    if (r->pos >= r->len) {
        if (!reader_fill(r)) return -1;
    }
    return r->buf[r->pos++];
}

static uint32_t reader_tell(const reader_t *r) {
    return (uint32_t)(r->buf_file_offset + r->pos);
}

static bool reader_seek_abs(reader_t *r, uint32_t abs_pos) {
    r->pos = r->len = 0;
    return f_lseek(r->fp, abs_pos) == FR_OK;
}

static bool reader_skip(reader_t *r, uint32_t n) {
    return reader_seek_abs(r, reader_tell(r) + n);
}

// --- wait/timing --------------------------------------------------------

static absolute_time_t s_song_start;
static uint64_t s_samples_elapsed;

// Whole-song sample count, NOT reset by wait_reset() (i.e. it runs straight
// through loop points and the Sega PCM upload re-baseline), so the OLED can
// show a monotonically rising elapsed time. Published as seconds in one
// 32-bit word for lock-free cross-core reads (see vgm_player_elapsed_seconds).
static uint32_t s_song_samples;
static volatile uint32_t s_elapsed_seconds;

// Clock preset chosen for each chip at song start (see vgm_player_play).
// Re-asserted once per song-second by reassert_clocks() so a slave that
// wasn't listening on SPI during the one-shot RESET burst (still booting, or
// reflashed mid-session) self-corrects its render rate instead of playing
// the whole song at its power-on default (a uniform pitch/tempo shift).
static uint8_t s_chip_preset[VGM_CHIP_COUNT];

static void reassert_clocks(void) {
    // Only the chips whose slave renders at a clock-derived native rate --
    // SN76489/AY-3-8910/SCC resample to a fixed output rate, so their
    // sample_rate_hz never changes and there is nothing to correct.
    static const vgm_chip_id_t clocked[] = {
        VGM_CHIP_YM2413, VGM_CHIP_YM2612, VGM_CHIP_YM2151,
        VGM_CHIP_YM2203, VGM_CHIP_SEGAPCM,
    };
    for (unsigned i = 0; i < sizeof clocked / sizeof clocked[0]; i++)
        slave_bus_set_clock(clocked[i], s_chip_preset[clocked[i]]);
}

uint32_t vgm_player_elapsed_seconds(void) {
    return s_elapsed_seconds;
}

static void wait_reset(void) {
    s_song_start = get_absolute_time();
    s_samples_elapsed = 0;
}

static void wait_samples(uint32_t n) {
    s_samples_elapsed += n;
    s_song_samples += n;
    uint32_t es = s_song_samples / VGM_SAMPLE_HZ;
#if PICO_ON_DEVICE
    // Print a MM:SS tick every 2 s of song time so a listener can note the
    // elapsed time of anything they hear (e.g. a spot where two chips drift)
    // without needing the OLED. One short line per 2 s -- negligible against
    // wait_samples()'s pacing (unlike VGM_MASTER_VERBOSE_LOG's per-command
    // spam).
    static uint32_t s_last_time_log = 0xFFFFFFFFu;
    if (es != s_elapsed_seconds && (es % 2) == 0 && es != s_last_time_log) {
        s_last_time_log = es;
        printf("[TIME] %02u:%02u\n", (unsigned)(es / 60), (unsigned)(es % 60));
    }
#endif
    if (es != s_elapsed_seconds) {
        // Once per song-second: re-assert every chip's clock preset without
        // resetting it, so a slave that missed the song-start RESET burst
        // corrects its render rate. Non-destructive and a no-op on the slave
        // when the rate already matches. See VGMSPI_OP_CLOCK.
        reassert_clocks();
    }
    s_elapsed_seconds = es;
    uint64_t target_us = s_samples_elapsed * 1000000ull / VGM_SAMPLE_HZ;
    absolute_time_t deadline = delayed_by_us(s_song_start, target_us);

#if VGM_MASTER_DEBUG_TRACE
    // How far behind the song's own schedule is the master right now? >0 =
    // late (a dense burst of register frames that took longer to send than
    // the musical time between them). Recovered on the next sparse wait.
    // Prints [LAG] MM:SS.mmm +Nms each time the backlog GROWS by >=3ms, and
    // stops once it plateaus / recovers -- so a stall shows up as a short
    // rising run of lines pinpointing where and how much the music slips.
    // (Its own printf adds ~1-2ms to the backlog while a stall is ongoing,
    // so consecutive numbers read a little high; the first spike is clean.)
    int64_t lag_us = absolute_time_diff_us(deadline, get_absolute_time());
    static int64_t s_lag_peak_us = 0;
    if (lag_us < 2000) {
        s_lag_peak_us = 0;
    } else if (lag_us > s_lag_peak_us + 3000) {
        s_lag_peak_us = lag_us;
        uint32_t frac_ms = (uint32_t)((s_song_samples % VGM_SAMPLE_HZ) * 1000u / VGM_SAMPLE_HZ);
        printf("[LAG] %02u:%02u.%03u  +%lldms\n",
               (unsigned)(es / 60), (unsigned)(es % 60), (unsigned)frac_ms,
               (long long)(lag_us / 1000));
    }
#endif

    sleep_until(deadline);
}

#if VGM_MASTER_DEBUG_TRACE
// One-shot "first time this kind of event appears in the stream" log. Used to
// read, straight off the file, the song-time at which each voice first speaks
// (e.g. AY channel A = the melody vs. AY noise = the drums) -- so a "the
// melody comes in a beat late" complaint can be checked against where the rip
// actually places the note, independent of any playback timing. `slot` is a
// small unique id (0..31); each fires at most once per song.
static void first_event_log(unsigned slot, const char *what) {
    static uint32_t s_fired_mask = 0;
    if (slot >= 32 || (s_fired_mask & (1u << slot))) return;
    s_fired_mask |= (1u << slot);
    uint32_t es = s_song_samples / VGM_SAMPLE_HZ;
    uint32_t frac_ms = (uint32_t)((s_song_samples % VGM_SAMPLE_HZ) * 1000u / VGM_SAMPLE_HZ);
    printf("[FIRST] %-16s %02u:%02u.%03u\n", what,
           (unsigned)(es / 60), (unsigned)(es % 60), (unsigned)frac_ms);
}

// Note-onset trace, windowed to VGM_EV_LO_MS..VGM_EV_HI_MS of song time, so we
// can read the AY melody's retriggers (R13) against the SCC's key-ons straight
// off the stream around the spot the listener flags -- if they line up here but
// sound offset, the desync is not in the file; if they're already offset here,
// it is the rip/arrangement.
#ifndef VGM_EV_LO_MS
#define VGM_EV_LO_MS 7000u
#endif
#ifndef VGM_EV_HI_MS
#define VGM_EV_HI_MS 13000u
#endif
static void ev_trace(const char *what, uint8_t a, uint8_t d) {
    uint32_t ms = (uint32_t)((uint64_t)s_song_samples * 1000u / VGM_SAMPLE_HZ);
    if (ms < VGM_EV_LO_MS || ms > VGM_EV_HI_MS) return;
    printf("[EV] %02u.%03u %-11s a=%02X d=%02X\n",
           (unsigned)(ms / 1000), (unsigned)(ms % 1000), what, a, d);
}
#endif

// --- header -------------------------------------------------------------

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A VGM header clock field at byte offset `off` (4 bytes) is only really part
// of the header when the VGM data stream starts at or after off+4. If the
// data starts earlier (e.g. a v1.50 Mega Drive rip: SN76489 + YM2612, data
// at 0x40), those 4 bytes ARE the command stream -- reading them as a clock
// invents a chip. VGM spec: "a VGM file with a data offset of 0x40 only has a
// header up to offset 0x3F." Concretely this is what made "04 - Water.vgm"
// (MD, no YM2203) light up YM2203 on the OLED: its first two YM2612 register
// writes (0x52 0x28 0x00 0x52 ...) sit at 0x44, the YM2203-clock slot.
// Returns 0 for an absent/out-of-header field; masks off the dual-chip flag.
static uint32_t hdr_clock(const uint8_t *header, uint32_t data_start, uint32_t off) {
    if (data_start < off + 4) return 0;
    return rd_u32le(header + off) & 0x7FFFFFFFu;
}

// Bitmask of the chips a song uses, read from its (already loaded,
// zero-padded to 0x100) VGM header: bit (1u << <vgm_chip_id_t>) is set when
// that chip's clock field is populated (non-zero and inside the header, see
// hdr_clock). Version-gated per VGM spec. This is the set reported to
// vgm_player_opts_t.on_chips and used by vgm_player_scan_chips().
static uint32_t header_chip_mask(const uint8_t *header) {
    uint32_t version = rd_u32le(header + 0x08);
    uint32_t data_rel = rd_u32le(header + 0x34);
    uint32_t data_start = (version >= 0x150 && data_rel != 0) ? (0x34 + data_rel) : 0x40;
    uint32_t mask = 0;
    if (hdr_clock(header, data_start, 0x0C)) mask |= 1u << VGM_CHIP_SN76489;
    if (hdr_clock(header, data_start, 0x10)) mask |= 1u << VGM_CHIP_YM2413;
    if (hdr_clock(header, data_start, 0x2C)) mask |= 1u << VGM_CHIP_YM2612;
    if (version >= 0x110 && hdr_clock(header, data_start, 0x30)) mask |= 1u << VGM_CHIP_YM2151;
    if (version >= 0x151 && hdr_clock(header, data_start, 0x44)) mask |= 1u << VGM_CHIP_YM2203;
    if (version >= 0x151 && hdr_clock(header, data_start, 0x74)) mask |= 1u << VGM_CHIP_AY8910;
    if (version >= 0x151 && hdr_clock(header, data_start, 0x38)) mask |= 1u << VGM_CHIP_SEGAPCM;
    if (version >= 0x161 && hdr_clock(header, data_start, 0x9C)) mask |= 1u << VGM_CHIP_SCC;
    return mask;
}

bool vgm_player_scan_chips(const char *path, uint32_t *out_mask) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;
    uint8_t header[0x100];
    memset(header, 0, sizeof(header));
    UINT br = 0;
    f_read(&file, header, sizeof(header), &br);
    f_close(&file);
    if (br < 0x40 || memcmp(header, "Vgm ", 4) != 0) return false;
    if (out_mask) *out_mask = header_chip_mask(header);
    return true;
}

// --- unsupported-command skipping ---------------------------------------
//
// Every VGM command we don't act on must still be consumed at the right
// byte length so the parser stays in sync with the stream. Lengths below
// follow VGM spec 1.71; genuinely unrecognized opcodes abort the song
// rather than guess (see vgm_player.h / docs/design-notes.md).
static bool skip_command(reader_t *rd, uint8_t cmd) {
    if (cmd >= 0x30 && cmd <= 0x3F) return reader_skip(rd, 1); // dual-chip/reserved 1-byte family
    if (cmd >= 0x40 && cmd <= 0x4E) return reader_skip(rd, 2); // Mikey/K007232/K005289/MSM5232/ICS2115/reserved
    if (cmd == 0x4F) return reader_skip(rd, 1);                // GG PSG stereo
    if (cmd >= 0x56 && cmd <= 0x5F) return reader_skip(rd, 2); // other FM chips (aa,dd)
    if (cmd >= 0xA1 && cmd <= 0xAF) return reader_skip(rd, 2);
    if (cmd >= 0xB0 && cmd <= 0xBF) return reader_skip(rd, 2);
    if (cmd >= 0xC0 && cmd <= 0xDF) return reader_skip(rd, 3);
    if (cmd == 0xE0 || cmd == 0xE1) return reader_skip(rd, 4);
    if (cmd >= 0xE2 && cmd <= 0xFF) return reader_skip(rd, 4);
    switch (cmd) {
        case 0x90: return reader_skip(rd, 4);
        case 0x91: return reader_skip(rd, 4);
        case 0x92: return reader_skip(rd, 5);
        case 0x93: return reader_skip(rd, 10);
        case 0x94: return reader_skip(rd, 1);
        case 0x95: return reader_skip(rd, 4);
        default: return false; // truly unknown opcode: caller stops
    }
}

// --- Sega PCM ROM-window pre-scan -------------------------------------------
//
// Return the lowest 0x67 type-0x80 (Sega PCM ROM) block start address in the
// song, rounded down to 64KB -- the coarsest Sega PCM bank size, so the
// rounding never splits a bank. This is the ROM-window base the slave
// subtracts from every sample address, which lets a song whose samples sit
// high in a large ROM (OutRun's is 512KB) still fit the slave's fixed 192KB
// buffer as long as the bytes it actually references span less than that.
// Returns 0 when the song has no such block (then the slave does no
// rebasing).
//
// Every VGM ripper emits all data blocks back-to-back at the very start of
// the data, before the first wait/register command. So this reads only that
// leading run of 0x67 blocks and stops at the first other byte -- it must
// NOT walk the whole ~300KB command stream (doing that with a seek per
// command was minutes of SD I/O).
static uint32_t prescan_rom_base(FIL *fp, uint32_t data_start) {
    reader_t rd = {.fp = fp};
    if (!reader_seek_abs(&rd, data_start)) return 0;

    uint32_t min_start = UINT32_MAX;
    for (;;) {
        if (reader_byte(&rd) != 0x67) break; // end of the leading block run (or EOF/0x66)
        if (reader_byte(&rd) != 0x66) break; // malformed block
        int type = reader_byte(&rd);
        if (type < 0) break;
        uint8_t szb[4];
        bool got = true;
        for (int i = 0; i < 4; i++) {
            int b = reader_byte(&rd);
            if (b < 0) { got = false; break; }
            szb[i] = (uint8_t)b;
        }
        if (!got) break;
        uint32_t size = rd_u32le(szb);
        if (type == 0x80 && size >= 8) {
            uint8_t hdr[8];
            for (int i = 0; i < 8; i++) {
                int b = reader_byte(&rd);
                if (b < 0) { got = false; break; }
                hdr[i] = (uint8_t)b;
            }
            if (!got) break;
            uint32_t start = rd_u32le(hdr + 4);
            if (start < min_start) min_start = start;
            if (!reader_skip(&rd, size - 8)) break;
        } else if (!reader_skip(&rd, size)) {
            break;
        }
    }

    if (min_start == UINT32_MAX) return 0;
    return min_start & ~0xFFFFu;
}

// 0x67 0x66 tt <4-byte LE size> <size bytes of data>: PCM/ROM data block.
// type 0x80 (Sega PCM ROM data) gets streamed to the Sega PCM slave (see
// VGMSPI_OP_PCM_UPLOAD_*); every other type is just skipped -- we don't
// have a slave that plays it back, but we still have to consume exactly
// `size` bytes to stay in sync with the stream.
//
// A ROM-image block (VGM spec, data types 0x80-0xBF) is NOT raw ROM bytes:
// its `size` bytes begin with an 8-byte prefix --
//   [0..3] uint32  total ROM size   (ignored: our slave has a fixed
//                                    SEGAPCM_ROM_BUDGET buffer)
//   [4..7] uint32  start address of THIS chunk within the ROM
//   [8..]  the actual ROM bytes
// -- and a song may ship several such blocks covering different regions.
// The Sega PCM channel address registers (VGM 0xC0) address the assembled
// ROM image absolutely, so each chunk has to land at its own start
// address. The earlier code streamed the whole block verbatim from cursor
// 0, which wrote the 8 prefix bytes in as if they were samples and ignored
// start_addr entirely -- any rip that wasn't a single chunk based at 0
// then played back as clicks/noise.
static bool handle_data_block(reader_t *rd) {
    int marker = reader_byte(rd); // must be 0x66
    if (marker != 0x66) return false;
    int type = reader_byte(rd);
    if (type < 0) return false;
    uint8_t szbuf[4];
    for (int i = 0; i < 4; i++) {
        int b = reader_byte(rd);
        if (b < 0) return false;
        szbuf[i] = (uint8_t)b;
    }
    uint32_t size = rd_u32le(szbuf);

    if (type == 0x80) {
        if (size < 8) return false; // malformed ROM-image block
        uint8_t hdr[8];
        for (int i = 0; i < 8; i++) {
            int b = reader_byte(rd);
            if (b < 0) return false;
            hdr[i] = (uint8_t)b;
        }
        uint32_t start_addr = rd_u32le(hdr + 4);
        // Seek the slave's upload cursor to this chunk's start address.
        // VGMSPI_OP_PCM_UPLOAD_RESET only carries addr[23:16] and addr[15:8]
        // (no room in a 3-byte frame for addr[7:0]), so the seek itself is
        // 256-byte granular. Tightly-packed rips DON'T 256-align their chunk
        // starts (e.g. OutRun's "Magical Sound Shower" has 12 chunks with
        // starts like 0x040090, 0x0429AB) -- an earlier assumption that they
        // always would left every such song's samples shifted by <256 bytes,
        // heard as continuous crackle once the song was audible at all. Fix:
        // seek to the 256-aligned page and pad the sub-256 remainder with
        // 0x80 (Sega PCM digital silence) so the real data lands at the exact
        // byte offset. Costs <256 extra upload frames per chunk.
        uint32_t pad = start_addr & 0xFFu;
#if VGM_MASTER_VERBOSE_LOG
        printf("[VGM ] 0x67 SegaPCM ROM block: %lu data bytes @ ROM 0x%lX (pad %lu)\n",
               (unsigned long)(size - 8), (unsigned long)start_addr, (unsigned long)pad);
#endif
        slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_PCM_UPLOAD_RESET,
                       (uint8_t)(start_addr >> 16), (uint8_t)(start_addr >> 8));
        for (uint32_t i = 0; i < pad; i++)
            slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_PCM_UPLOAD_BYTE, 0, 0x80);
        for (uint32_t i = 8; i < size; i++) {
            int b = reader_byte(rd);
            if (b < 0) return false;
            slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_PCM_UPLOAD_BYTE, 0, (uint8_t)b);
        }
        // The upload above is ~65us per byte on real hardware -- tens of KB
        // of ROM means SECONDS of wall-clock time spent here. Sega PCM ROM
        // blocks sit at the very top of the data stream, before any wait
        // command, so musical time-zero is now, not when wait_reset() ran
        // before the parse loop. Without this re-baseline the first
        // wait_samples() sees its deadline already many seconds in the past
        // and the player races through the opening of the song to "catch
        // up" -- i.e. playback starts partway in. (The slaves do the
        // equivalent resync on their side; see slave_engine.c.)
        wait_reset();
        return true;
    }
    return reader_skip(rd, size);
}

bool vgm_player_play(const char *path, const vgm_player_opts_t *opts) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        printf("  ERROR: could not open %s\n", path);
        return false;
    }

    uint8_t header[0x100]; // full VGM 1.71 header; unread tail stays zeroed
    memset(header, 0, sizeof(header));
    UINT br = 0;
    f_read(&file, header, sizeof(header), &br);
    if (br < 0x40 || memcmp(header, "Vgm ", 4) != 0) {
        printf("  ERROR: %s is not a valid VGM file (bad header)\n", path);
        f_close(&file);
        return false;
    }

    uint32_t version = rd_u32le(header + 0x08);
    uint32_t data_rel = rd_u32le(header + 0x34);
    uint32_t data_start = (version >= 0x150 && data_rel != 0) ? (0x34 + data_rel) : 0x40;
    uint32_t loop_rel = rd_u32le(header + 0x1C);
    uint32_t loop_abs = (loop_rel != 0) ? (0x1C + loop_rel) : 0;

    // hdr_clock() returns 0 for any field that lies past the real end of this
    // file's header (data_start), so a Mega Drive rip with its command stream
    // beginning at 0x40 doesn't get a phantom YM2203 (0x44) / AY-3-8910 (0x74)
    // etc. See hdr_clock()'s comment.
    uint32_t sn76489_clock = hdr_clock(header, data_start, 0x0C);
    uint32_t ym2413_clock  = hdr_clock(header, data_start, 0x10);
    uint32_t ym2612_clock  = hdr_clock(header, data_start, 0x2C);
    uint32_t ym2151_clock  = (version >= 0x110) ? hdr_clock(header, data_start, 0x30) : 0;
    uint32_t segapcm_clock = (version >= 0x151) ? hdr_clock(header, data_start, 0x38) : 0;
    uint32_t segapcm_intf  = (version >= 0x151 && data_start >= 0x40) ? rd_u32le(header + 0x3C) : 0; // Sega PCM interface (bank) register
    uint32_t ym2203_clock  = (version >= 0x151) ? hdr_clock(header, data_start, 0x44) : 0;
    uint32_t ay8910_clock  = (version >= 0x151) ? hdr_clock(header, data_start, 0x74) : 0;
    // K051649/SCC clock (0x9C, VGM >= 1.61): would drive which SCC_CLOCK_PRESETS
    // entry the slave uses (see the s_chip_preset[VGM_CHIP_SCC] note below).
    // Whether a song uses SCC at all now comes from header_chip_mask().
    uint32_t scc_clock = (version >= 0x161) ? hdr_clock(header, data_start, 0x9C) : 0;

    // Every chip below whose slave renders at its own clock-derived native
    // rate instead of resampling (all except SN76489/AY-3-8910/SCC) needs
    // its ACTUAL VGM-file clock translated into a preset, not a hardcoded
    // 0 -- getting this wrong doesn't crash or glitch, it just shifts pitch
    // and tempo uniformly for the whole song. See chip_ops_t.sample_rate_hz's
    // doc comment in slave_common/include/slave_engine.h.
    s_chip_preset[VGM_CHIP_SN76489] = vgm_pick_clock_preset(sn76489_clock, SN76489_CLOCK_PRESETS, 3);
    s_chip_preset[VGM_CHIP_YM2413]  = vgm_pick_clock_preset(ym2413_clock, YM2413_CLOCK_PRESETS, 2);
    s_chip_preset[VGM_CHIP_YM2612]  = vgm_pick_clock_preset(ym2612_clock, YM2612_CLOCK_PRESETS, 2);
    s_chip_preset[VGM_CHIP_AY8910]  = vgm_pick_clock_preset(ay8910_clock, AY8910_CLOCK_PRESETS, 3);
    s_chip_preset[VGM_CHIP_YM2151]  = vgm_pick_clock_preset(ym2151_clock, YM2151_CLOCK_PRESETS, 2);
    s_chip_preset[VGM_CHIP_YM2203]  = vgm_pick_clock_preset(ym2203_clock, YM2203_CLOCK_PRESETS, 2);
    // Always preset 0 = 3.58 MHz: on real hardware the user confirmed the
    // 1789772 the header sometimes carries plays an octave too low, i.e. the
    // rips' period values are for the full MSX clock. The threading table +
    // slave support stay in place -- swap the 0 below for
    // vgm_pick_clock_preset(scc_clock, SCC_CLOCK_PRESETS, 2) if a file
    // genuinely authored for the half clock ever turns up.
    (void)SCC_CLOCK_PRESETS;
    (void)scc_clock;
    s_chip_preset[VGM_CHIP_SCC]     = 0;
    s_chip_preset[VGM_CHIP_SEGAPCM] = vgm_pick_clock_preset(segapcm_clock, SEGAPCM_CLOCK_PRESETS, 3);

    for (int c = 0; c < VGM_CHIP_COUNT; c++)
        slave_bus_reset((vgm_chip_id_t)c, s_chip_preset[c]);
    // Belt-and-braces: a slave still booting through the one-shot RESET burst
    // above stays at its power-on default rate for the whole song. The same
    // presets are re-asserted (non-destructively) once per song-second from
    // wait_samples() so a late slave self-corrects -- see reassert_clocks().
    // Sega PCM ROM-window base: pre-scan the song's 0x67 type-0x80 blocks
    // for their lowest start address so the slave can store a high-ROM song
    // (e.g. OutRun, 512KB ROM) within its fixed budget. 0 = no rebasing.
    // See VGMSPI_OP_SEGAPCM_ROM_BASE. Sent before any UPLOAD_RESET/BYTE.
    uint32_t segapcm_rom_base = prescan_rom_base(&file, data_start);
    slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_SEGAPCM_ROM_BASE,
                   (uint8_t)(segapcm_rom_base >> 16), (uint8_t)(segapcm_rom_base >> 8));
    // Start every song with a fresh Sega PCM ROM upload cursor at 0, so a
    // song's own 0x67 type-0x80 data block(s) (handled below, in the main
    // loop) always land at the addresses it expects.
    slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_PCM_UPLOAD_RESET, 0, 0);
    // Sega PCM bank config from the VGM header's interface register (0x3C),
    // decoded the libvgm/MAME way: low byte = bankshift, byte 2 = bankmask
    // pattern (OR'd with 0x70 = the BANK_MASK7 default). The slave applies
    // its own defaults (shift 12, mask 0x70) when we send 0. Sent before the
    // ROM upload so the slave knows how CTRL-register bank bits map.
    {
        uint8_t bankshift = (uint8_t)(segapcm_intf & 0xFF);
        uint8_t bankmask = (segapcm_intf != 0)
                         ? (uint8_t)(0x70 | ((segapcm_intf >> 16) & 0xFC))
                         : 0; // 0 -> slave uses its default
        slave_bus_send(VGM_CHIP_SEGAPCM, VGMSPI_OP_SEGAPCM_BANK, bankshift, bankmask);
    }

    if (opts && opts->on_chips)
        opts->on_chips(header_chip_mask(header));

    reader_t rd = {.fp = &file};
    if (!reader_seek_abs(&rd, data_start)) {
        printf("  ERROR: could not seek to VGM data start (offset 0x%lX) in %s\n",
               (unsigned long)data_start, path);
        f_close(&file);
        return false;
    }

    s_song_samples = 0; // measure elapsed time from here (not reset on loop)
    s_elapsed_seconds = 0;
    wait_reset();
    bool skip_requested = false;
    bool ok = true;
    uint32_t loop_plays = 1; // times the loop region has been entered (the
                             // first play-through counts as 1)

    for (;;) {
        int cmd = reader_byte(&rd);
        if (cmd < 0) break; // ran off the end without a proper 0x66 -- treat as EOF

        switch (cmd) {
            case 0x50: {
                int dd = reader_byte(&rd);
                if (dd < 0) { ok = false; break; }
#if VGM_MASTER_VERBOSE_LOG
                printf("[VGM ] 0x50 dd=0x%02X\n", (uint8_t)dd);
#endif
                slave_bus_write(VGM_CHIP_SN76489, 0, 0, (uint8_t)dd);
                break;
            }
            case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: {
                int aa = reader_byte(&rd), dd = reader_byte(&rd);
                if (aa < 0 || dd < 0) { ok = false; break; }
#if VGM_MASTER_VERBOSE_LOG
                printf("[VGM ] 0x%02X aa=0x%02X dd=0x%02X\n", (uint8_t)cmd, (uint8_t)aa, (uint8_t)dd);
#endif
                vgm_chip_id_t chip;
                switch (cmd) {
                    case 0x51: chip = VGM_CHIP_YM2413; break;
                    case 0x54: chip = VGM_CHIP_YM2151; break;
                    case 0x55: chip = VGM_CHIP_YM2203; break;
                    default:   chip = VGM_CHIP_YM2612; break; // 0x52/0x53
                }
                uint8_t port = (cmd == 0x53) ? 1 : 0;
                slave_bus_write(chip, port, (uint8_t)aa, (uint8_t)dd);
                break;
            }
            case 0xA0: {
                int aa = reader_byte(&rd), dd = reader_byte(&rd);
                if (aa < 0 || dd < 0) { ok = false; break; }
#if VGM_MASTER_VERBOSE_LOG
                printf("[VGM ] 0xA0 aa=0x%02X dd=0x%02X\n", (uint8_t)aa, (uint8_t)dd);
#endif
#if VGM_MASTER_DEBUG_TRACE
                switch (aa & 0x0F) {
                    case 0: case 1:  first_event_log(0, "AY chA tone");  break;
                    case 2: case 3:  first_event_log(1, "AY chB tone");  break;
                    case 4: case 5:  first_event_log(2, "AY chC tone");  break;
                    case 6:          first_event_log(3, "AY noise");     break;
                    case 7:          first_event_log(4, "AY mixer");     break;
                    case 13:         first_event_log(5, "AY env-shape"); break;
                    default: break;
                }
                if ((aa & 0x0F) >= 8 && (aa & 0x0F) <= 10 && (dd & 0x10))
                    first_event_log(6, "AY env-enable");
                if ((aa & 0x0F) == 13) ev_trace("AY env-shape", (uint8_t)aa, (uint8_t)dd);
                // R11/R12 = envelope-period low/high. Track both bytes and, in
                // the window, print the resulting 16-bit EP: decay is 32*EP
                // clock/8 ticks per step (~110ms/step at EP=768, ~15ms at
                // EP=102), i.e. tells whether the 0:09 notes are near-flat
                // sustains or real plucks.
                if ((aa & 0x0F) == 11 || (aa & 0x0F) == 12) {
                    static uint8_t s_ep_lo, s_ep_hi;
                    if ((aa & 0x0F) == 11) s_ep_lo = (uint8_t)dd; else s_ep_hi = (uint8_t)dd;
                    uint32_t ms = (uint32_t)((uint64_t)s_song_samples * 1000u / VGM_SAMPLE_HZ);
                    if (ms >= VGM_EV_LO_MS && ms <= VGM_EV_HI_MS)
                        printf("[EV] %02u.%03u AY env-per   R%u=%02X  EP=%u\n",
                               (unsigned)(ms / 1000), (unsigned)(ms % 1000),
                               (unsigned)(aa & 0x0F), (uint8_t)dd,
                               (unsigned)(((uint16_t)s_ep_hi << 8) | s_ep_lo));
                }
#endif
                slave_bus_write(VGM_CHIP_AY8910, 0, (uint8_t)(aa & 0x0F), (uint8_t)dd);
                break;
            }
            case 0xD2: {
                // K051649/SCC: 0xD2 pp aa dd. Real hardware exposes this as
                // a 2-step latch+write across 5 regions selected by `pp`;
                // we pre-decode the region into a dedicated opcode instead
                // of replicating that handshake over the wire -- see
                // VGMSPI_OP_SCC_* in protocol/vgm_spi_protocol.h.
                int pp = reader_byte(&rd), aa = reader_byte(&rd), dd = reader_byte(&rd);
                if (pp < 0 || aa < 0 || dd < 0) { ok = false; break; }
#if VGM_MASTER_VERBOSE_LOG
                printf("[VGM ] 0xD2 pp=0x%02X aa=0x%02X dd=0x%02X\n", (uint8_t)pp, (uint8_t)aa, (uint8_t)dd);
#endif
                uint8_t opcode;
                switch (pp & 0x7F) {
                    case 0x00: case 0x04: opcode = VGMSPI_OP_SCC_WAVEFORM; break;
                    case 0x01: opcode = VGMSPI_OP_SCC_FREQ; break;
                    case 0x02: opcode = VGMSPI_OP_SCC_VOLUME; break;
                    case 0x03: opcode = VGMSPI_OP_SCC_KEYON; break;
                    default: opcode = 0; break; // test register / unknown: drop
                }
#if VGM_MASTER_DEBUG_TRACE
                if (opcode == VGMSPI_OP_SCC_FREQ)   first_event_log(10, "SCC freq");
                if (opcode == VGMSPI_OP_SCC_KEYON && dd) first_event_log(11, "SCC key-on");
                if (opcode == VGMSPI_OP_SCC_KEYON) ev_trace("SCC key-on", (uint8_t)aa, (uint8_t)dd);
#endif
                if (opcode) slave_bus_send(VGM_CHIP_SCC, opcode, (uint8_t)aa, (uint8_t)dd);
                break;
            }
            case 0x61: {
                int n0 = reader_byte(&rd), n1 = reader_byte(&rd);
                if (n0 < 0 || n1 < 0) { ok = false; break; }
                wait_samples((uint32_t)n0 | ((uint32_t)n1 << 8));
                break;
            }
            case 0x62: wait_samples(735); break;
            case 0x63: wait_samples(882); break;
            case 0x66:
                if (opts && opts->loop_enabled && loop_abs != 0 &&
                    (opts->max_loops == 0 || loop_plays < opts->max_loops)) {
                    loop_plays++;
                    if (!reader_seek_abs(&rd, loop_abs)) { ok = false; break; }
                    wait_reset();
                    continue; // don't fall through to the poll_skip check's EOF logic
                }
                goto done; // no loop point, looping disabled, or loop budget spent
            case 0x67:
                if (!handle_data_block(&rd)) { ok = false; }
                break;
            case 0xC0: {
                // Sega PCM register write: 0xC0 bbaa dd (16-bit LE offset).
                // Our slave's meaningful register footprint fits an 8-bit
                // index (see slave_segapcm/src/chip_segapcm.c), so only the
                // low byte of the offset matters.
                int b0 = reader_byte(&rd), b1 = reader_byte(&rd), dd = reader_byte(&rd);
                if (b0 < 0 || b1 < 0 || dd < 0) { ok = false; break; }
#if VGM_MASTER_VERBOSE_LOG
                printf("[VGM ] 0xC0 b0=0x%02X b1=0x%02X dd=0x%02X\n", (uint8_t)b0, (uint8_t)b1, (uint8_t)dd);
#endif
                slave_bus_write(VGM_CHIP_SEGAPCM, 0, (uint8_t)b0, (uint8_t)dd);
                break;
            }
            default:
                if (cmd >= 0x70 && cmd <= 0x7F) {
                    wait_samples((uint32_t)(cmd & 0x0F) + 1);
                } else if (cmd >= 0x80 && cmd <= 0x8F) {
                    // YM2612 DAC-from-PCM-bank write: we don't stream PCM in
                    // this build (see docs/design-notes.md), so just honor
                    // the implicit wait.
                    wait_samples((uint32_t)(cmd & 0x0F));
                } else if (!skip_command(&rd, (uint8_t)cmd)) {
                    ok = false;
                }
                break;
        }

        if (!ok) break;
        if (opts && opts->poll_skip && opts->poll_skip()) { skip_requested = true; break; }
    }

done:
    slave_bus_mute_all();
    f_close(&file);
    return ok || skip_requested;
}
