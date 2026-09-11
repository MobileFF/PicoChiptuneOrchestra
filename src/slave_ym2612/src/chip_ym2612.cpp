#include "chip_ym2612.h"
#include "ymfm_opn.h"
#include <string.h>

namespace {

// Default virtual methods (no ADPCM external memory, no timer/IRQ
// consumers) are sufficient: this slave only needs FM register writes in
// and PCM samples out.
class MinimalInterface : public ymfm::ymfm_interface {};

MinimalInterface s_intf;
ymfm::ym2612 s_chip(s_intf);

// Must stay index-aligned with master/src/vgm_player.c's own copy.
constexpr uint32_t CLOCK_PRESETS[2] = {7670454, 7600489}; // NTSC, PAL

// --- Mega Drive "PCM" = YM2612 channel-6 DAC, fed from an uploaded bank ---
// The master uploads the VGM's 0x67 type-0 PCM data (all blocks concatenated)
// here once. Playback is checkpoint-driven (see vgm_spi_protocol.h): DAC_START
// snaps an absolute read position, DAC_SYNC (every ~2ms of PCM) carries the
// master's current position coarsened /4. On each DAC_SYNC this code measures
// how many of ITS OWN output samples elapsed since the last one and derives
// bank-bytes-per-output-sample -- so the 44100 Hz VGM clock, the ~53267 Hz
// render clock, and any 0x8n `n` jitter all cancel -- then phase-locks. Every
// render, the fractional read position advances by that rate and each whole
// byte it crosses is written to YM2612 reg 0x2A (ymfm holds it -- ZOH). DAC
// mode itself is enabled by the VGM's own `0x52 2B 80` write.
uint8_t  s_pcm[YM2612_PCM_BUDGET];
uint32_t s_pcm_wr;       // upload write cursor
uint32_t s_pcm_len;      // valid bank length (== s_pcm_wr after the burst)
uint8_t  s_dac_seek_hi;  // latched offset[23:16] for the next DAC_START
bool     s_dac_active;   // between DAC_START and DAC_STOP

uint64_t s_dac_play_pos;   // fractional bank read position, 48.16 fixed-point
uint32_t s_dac_read_idx;   // whole-byte index last written to reg 0x2A (monotonic)
uint32_t s_dac_ckpt_pos;   // last checkpoint's bank position (whole bytes)
uint32_t s_dac_rate;       // bank bytes per output sample, 16.16 fixed-point
uint32_t s_dac_local;      // free-running output-sample counter (render clock)
uint32_t s_dac_ckpt_local; // s_dac_local at the last DAC_START / DAC_SYNC

// ~14 kHz DAC / ~53267 Hz render = 0.263 bytes/sample; seeds s_dac_rate at the
// first DAC_START only (later runs keep the tracked rate) so the opening ~2ms
// before the first DAC_SYNC isn't frozen.
constexpr uint32_t DAC_RATE_SEED = (uint32_t)((14000.0 / 53267.0) * 65536.0);
// DAC_SYNC delivers this many 0x8n's worth of advance (master's DAC_SYNC_EVERY).
constexpr uint32_t DAC_SYNC_EVERY = 32;

} // namespace

extern "C" void ym2612_reset(uint8_t clock_preset) {
    // ymfm's FM engine has no per-instance clock setting to configure here
    // -- register values (FNUM/BLOCK) already encode pitch relative to a
    // fixed internal ratio, and the real-world clock only matters for
    // translating to real-world Hz via sample_rate(), i.e. for choosing the
    // correct render rate. See ym2612_sample_rate_hz() below, and
    // chip_ops_t.sample_rate_hz's doc comment for why this reset() is
    // genuinely clock-independent rather than "clock doesn't matter".
    (void)clock_preset;
    s_chip.reset();
    // Drop any DAC playback / bank from the previous song. The bank itself
    // isn't cleared: s_pcm_len gates reads, and the next song re-uploads
    // from cursor 0, so stale bytes past the new length are never read.
    s_pcm_wr = 0;
    s_pcm_len = 0;
    s_dac_seek_hi = 0;
    s_dac_active = false;
    s_dac_play_pos = 0;
    s_dac_read_idx = 0;
    s_dac_ckpt_pos = 0;
    s_dac_rate = 0;
    s_dac_local = 0;
    s_dac_ckpt_local = 0;
}

extern "C" uint32_t ym2612_sample_rate_hz(uint8_t clock_preset) {
    uint32_t clock = CLOCK_PRESETS[clock_preset < 2 ? clock_preset : 0];
    return s_chip.sample_rate(clock);
}

extern "C" void ym2612_write(uint8_t port, uint8_t reg, uint8_t data) {
    // Ports 0 (VGMSPI_OP_WRITE0) and 1 (WRITE1) are the chip's two real
    // register banks. Any other port value means slave_spi_rx.c's resync
    // misdispatched a frame with a dropped opcode byte (e.g. a {WRITE0,
    // reg=0x08, data} decoded as SCC_KEYON, opcode 0x08) -- drop it rather
    // than corrupt a register. See chip_ym2151.cpp / design-notes.md.
    if (port == 0) {
        s_chip.write(0, reg);
        s_chip.write(1, data);
    } else if (port == 1) {
        s_chip.write(2, reg);
        s_chip.write(3, data);
    } else if (port == 20) {          // PCM_RESET: reg:data = (upload cursor >> 2)
        uint32_t base = (((uint32_t)reg << 8) | data) << 2;
        s_pcm_wr = base;
        if (base == 0) s_pcm_len = 0;  // song-start reset also drops the old bank
    } else if (port == 21) {          // upload one PCM bank byte
        if (s_pcm_wr < YM2612_PCM_BUDGET) {
            s_pcm[s_pcm_wr++] = data;
            if (s_pcm_wr > s_pcm_len) s_pcm_len = s_pcm_wr;  // grow only
        }
    } else if (port == 22) {          // DAC_START: reg:data = absolute pos [15:0]
        uint32_t pos = ((uint32_t)s_dac_seek_hi << 16) | ((uint32_t)reg << 8) | data;
        s_dac_play_pos = (uint64_t)pos << 16;   // snap
        s_dac_read_idx = pos;
        s_dac_ckpt_pos = pos;
        s_dac_ckpt_local = s_dac_local;
        if (s_dac_rate == 0) s_dac_rate = DAC_RATE_SEED;
        s_dac_active = true;
        if (pos < s_pcm_len) {                  // start the run on the exact byte
            s_chip.write(0, 0x2A);
            s_chip.write(1, s_pcm[pos]);
        }
    } else if (port == 23) {          // DAC_SYNC: reg:data = (master pos >> 2) & 0xFFFF
        uint32_t coarse = (((uint32_t)reg << 8) | data) << 2;   // +/- 3 bytes of true
        uint32_t now = s_dac_local;
        uint32_t dt  = now - s_dac_ckpt_local;                  // render samples since last ckpt
        if (s_dac_active && coarse == s_dac_ckpt_pos && dt < 12) {
            return;                    // the master sends DAC_SYNC 2x; ignore the echo
        }
        s_dac_ckpt_local = now;
        s_dac_ckpt_pos = coarse;
        if (!s_dac_active || s_dac_rate == 0) {
            // Every burst DAC_START of this run was dropped on the lossy bus
            // (seen on the first PCM voice, right after the ~1s bank upload):
            // DAC_START never fired, so no rate is set and `dt` is measured
            // from boot, not the run start -- useless. Treat this checkpoint
            // as the anchor: activate, seed the rate, snap to `coarse`.
            s_dac_active = true;
            s_dac_rate = DAC_RATE_SEED;
            s_dac_play_pos = (uint64_t)coarse << 16;
            s_dac_read_idx = coarse;
        } else {
            if (dt) {
                // Master advanced DAC_SYNC_EVERY bytes over `dt` render samples
                // -> instantaneous rate; EMA-smooth (1/4) vs arrival jitter.
                uint32_t inst = (DAC_SYNC_EVERY << 16) / dt;
                s_dac_rate += ((int32_t)inst - (int32_t)s_dac_rate) >> 2;
            }
            // Phase: snap on a real desync; else only ever pull the read
            // position FORWARD (a backward read re-plays a byte and clicks),
            // and only when it has fallen well behind.
            int32_t err = (int32_t)coarse - (int32_t)(s_dac_play_pos >> 16);
            if (err > 96 || err < -96) {
                s_dac_play_pos = (uint64_t)coarse << 16;
                s_dac_read_idx = coarse;
            } else if (err > 12) {
                s_dac_play_pos += (uint64_t)((uint32_t)(err - 12)) << 13;
            }
        }
    } else if (port == 24) {          // DAC_STOP
        s_dac_active = false;
    } else if (port == 25) {          // DAC_SEEK: data = pos[23:16] for next DAC_START
        s_dac_seek_hi = data;
    } else if (port == 26) {          // DAC_RATE: reg:data = seed rate (16.16 bytes/sample)
        uint32_t r = ((uint32_t)reg << 8) | data;
        if (r) s_dac_rate = r;        // seed; the DAC_SYNC PLL refines it over the run
    }
}

extern "C" int16_t ym2612_render(void) {
    // Advance the DAC read position at the checkpoint-derived rate and write
    // reg 0x2A on each new whole byte. The read index is strictly monotonic
    // (never re-plays a byte -> no click). play_pos must NOT run past the last
    // checkpoint: the final byte(s) of a run go un-checkpointed and any runway
    // there coasts the read straight into the NEXT concatenated sample in the
    // bank -- a per-bar click on looped percussion. The rate measurement is
    // biased slightly low (sync frames arrive late), so with no runway play_pos
    // trails the checkpoint by a few bytes of latency and rarely hits the clamp.
    if (s_dac_active && s_dac_rate) {
        uint64_t limit = (uint64_t)s_dac_ckpt_pos << 16;
        if (s_dac_play_pos < limit) {
            s_dac_play_pos += s_dac_rate;
            if (s_dac_play_pos > limit) s_dac_play_pos = limit;
        }
        uint32_t want = (uint32_t)(s_dac_play_pos >> 16);
        if (want > s_dac_read_idx) {
            s_dac_read_idx = want;
            uint8_t b = (want < s_pcm_len) ? s_pcm[want]
                      : (s_pcm_len ? s_pcm[s_pcm_len - 1] : 0x80u);
            s_chip.write(0, 0x2A);
            s_chip.write(1, b);
        }
    }
    s_dac_local++;

    ymfm::ym2612::output_data out;
    s_chip.generate(&out, 1);
    // Measured range for a single loud channel is roughly +-5500 (see
    // tools/probes/ym2612_probe.cpp); summing L+R then >>3 (average, then
    // >>2) keeps typical passages comfortably inside audio_pwm's ~12-bit
    // headroom while the clamp below catches rare fortissimo peaks.
    int32_t mono = ((int32_t)out.data[0] + (int32_t)out.data[1]) >> 3;
    if (mono > 2047) mono = 2047;
    if (mono < -2048) mono = -2048;
    return (int16_t)mono;
}
