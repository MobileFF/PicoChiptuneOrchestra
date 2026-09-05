#include "slave_engine.h"

#include <stdio.h>
#include "pico/multicore.h"
#include "audio_pwm.h"
#include "vgm_spi_protocol.h"

// The once-per-second "RATE CHECK" line (does render() keep up with the
// sample rate in real time?) is compiled in for either VGM_SLAVE_VERBOSE_LOG
// (which also spams a printf per register write -- that spam itself steals
// enough core1 time to make the rate numbers meaningless) or the lighter
// VGM_SLAVE_RATE_CHECK, which adds ONLY that one line/sec, so the numbers
// reflect the real emulation cost.
#if VGM_SLAVE_VERBOSE_LOG || VGM_SLAVE_RATE_CHECK
#define RATE_CHECK_ON 1
#else
#define RATE_CHECK_ON 0
#endif

void slave_audio_engine_run(const chip_ops_t *ops, uint audio_pin, const char *chip_name) {
    audio_pwm_init(audio_pin);
    ops->reset(0);
    // Must come after reset(0) above, not before -- see this function's doc
    // comment in slave_engine.h.
    uint32_t sample_rate_hz = ops->sample_rate_hz(0);

    printf("[%s] audio engine running at %lu Hz\n", chip_name, (unsigned long)sample_rate_hz);

    bool muted = false;

    // Bresenham-style tick pacing: avoids a 64-bit division per sample
    // (sample_rate_hz doesn't divide 1e6 evenly, e.g. 44100) while keeping
    // long-run drift at zero -- the fractional remainder always carries
    // forward exactly rather than being truncated away each tick. Not
    // const: a RESET can select a different clock_preset, and for chips
    // that render at a clock-derived native rate instead of resampling
    // (see chip_ops_t.sample_rate_hz's doc comment) that changes
    // sample_rate_hz itself, so these need to be recomputed then too.
    uint32_t whole_us = 1000000u / sample_rate_hz;
    uint32_t rem_us = 1000000u % sample_rate_hz;
    uint32_t frac_acc = 0;

#if VGM_SLAVE_VERBOSE_LOG
    uint32_t pcm_upload_count = 0;
#endif
#if RATE_CHECK_ON
    // Does render() keep up with sample_rate_hz in real time on this
    // hardware? Falling behind is otherwise silent (the "resync to now"
    // below just quietly runs slower than intended), and manifests as a
    // uniform pitch/tempo drop with no other symptom. Reported ~once per
    // second of intended playback time.
    uint32_t rate_check_samples = 0;
    uint32_t rate_check_fell_behind = 0;
    uint32_t reset_rx = 0; // RESET frames received since boot -- 0 here means
                           // the master's one-shot clock-preset RESET never
                           // arrived and this chip is stuck at its power-on
                           // default rate (uniform pitch/tempo shift).
    absolute_time_t rate_check_start = get_absolute_time();
#endif

    absolute_time_t next = get_absolute_time();

    for (;;) {
        while (multicore_fifo_rvalid()) {
            uint32_t event = multicore_fifo_pop_blocking();
            uint8_t opcode = (uint8_t)(event >> 16);
            uint8_t reg = (uint8_t)(event >> 8);
            uint8_t data = (uint8_t)event;
            // FIX (2026-09-05): every opcode except NOP/MUTE means this chip
            // is being actively driven for the song now playing -- clear a
            // stale mute from the PREVIOUS song before the switch below.
            // Without this, the only way out of MUTE was a successfully
            // received VGMSPI_OP_RESET; RESET is a one-shot 3x-redundant
            // burst at song start (slave_bus.c), and on the rare occasion
            // ALL THREE are lost to an SPI hiccup, nothing else ever clears
            // `muted` -- register writes keep landing (this slave's own
            // ops->write() below runs fine, so the emulated chip's state is
            // correct) but audio_pwm_write() at the bottom of this loop
            // stays forced to 0 for the rest of that song. Reported as a VGM
            // that "plays" (per the master's log) but produces no sound at
            // all, non-deterministically -- the same file can hit this or
            // not depending on SPI timing, not on anything in the file
            // itself. VGMSPI_OP_CLOCK (sent once/song-second to 5 of the 8
            // chips to fix a missed RESET's pacing) never touched `muted`,
            // and the other 3 chips get no periodic message at all, so
            // neither had any self-recovery before this fix.
            if (opcode != VGMSPI_OP_NOP && opcode != VGMSPI_OP_MUTE) muted = false;
            switch (opcode) {
                case VGMSPI_OP_RESET:
                    ops->reset(reg);
                    muted = false;
                    // Re-pace the tick loop in case this preset's clock
                    // implies a different native render rate (ymfm-backed
                    // chips and Sega PCM; a no-op for chips that always
                    // resample to the same fixed output rate -- see
                    // chip_ops_t.sample_rate_hz's doc comment). Must run
                    // AFTER ops->reset() above, since sample_rate_hz() can
                    // depend on state reset() just (re)configured (e.g.
                    // YM2203's fidelity mode).
                    sample_rate_hz = ops->sample_rate_hz(reg);
                    whole_us = 1000000u / sample_rate_hz;
                    rem_us = 1000000u % sample_rate_hz;
                    frac_acc = 0;
                    next = get_absolute_time();
#if RATE_CHECK_ON
                    rate_check_samples = 0;
                    rate_check_fell_behind = 0;
                    reset_rx++;
                    rate_check_start = next;
#endif
                    printf("[%s] RESET preset=%u (%lu Hz)\n", chip_name, reg, (unsigned long)sample_rate_hz);
                    break;
                case VGMSPI_OP_CLOCK: {
                    // Re-assert the clock preset WITHOUT touching the chip.
                    // The master sends this once per song-second so a slave
                    // that missed the one-shot RESET at song start (still
                    // booting / reflashed mid-session) self-corrects instead
                    // of playing the whole song at its power-on default rate.
                    // Re-pace only if the rate actually changed, so the
                    // steady once-a-second frame is a true no-op.
                    uint32_t nr = ops->sample_rate_hz(reg);
                    if (nr != sample_rate_hz) {
                        sample_rate_hz = nr;
                        whole_us = 1000000u / sample_rate_hz;
                        rem_us = 1000000u % sample_rate_hz;
                        frac_acc = 0;
                        next = get_absolute_time();
#if RATE_CHECK_ON
                        rate_check_samples = 0;
                        rate_check_fell_behind = 0;
                        rate_check_start = next;
#endif
                        printf("[%s] CLOCK preset=%u (%lu Hz) -- late correction\n",
                               chip_name, reg, (unsigned long)sample_rate_hz);
                    }
                    break;
                }
                case VGMSPI_OP_WRITE0:
                    ops->write(0, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] WRITE0 reg=0x%02X data=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_WRITE1:
                    ops->write(1, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] WRITE1 reg=0x%02X data=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                // SCC-only opcodes: ports 2-5 are otherwise unused, so
                // routing them through the same generic write() is safe --
                // a given slave binary only ever receives the opcodes its
                // own chip_ops_t.write() was written to understand.
                case VGMSPI_OP_SCC_WAVEFORM:
                    ops->write(2, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SCC_WAVEFORM reg=0x%02X data=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_SCC_FREQ:
                    ops->write(3, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SCC_FREQ reg=0x%02X data=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_SCC_VOLUME:
                    ops->write(4, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SCC_VOLUME reg=0x%02X data=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_SCC_KEYON:
                    ops->write(5, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SCC_KEYON data=0x%02X\n", chip_name, data);
#endif
                    break;
                // Sega PCM-only opcodes: a whole song's sample ROM (tens of
                // KB, see slave_segapcm) arrives as a burst of
                // VGMSPI_OP_PCM_UPLOAD_BYTE events, one per byte -- see the
                // "fell behind" resync right after this loop, which is what
                // keeps that burst from causing a fast-forwarded catch-up
                // once it's drained. Logged as periodic progress rather
                // than per-byte even in verbose mode -- one line per byte
                // would be tens of thousands of lines for one song.
                case VGMSPI_OP_PCM_UPLOAD_RESET:
                    ops->write(6, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    pcm_upload_count = 0;
                    printf("[%s] PCM_UPLOAD_RESET\n", chip_name);
#endif
                    break;
                case VGMSPI_OP_PCM_UPLOAD_BYTE:
                    ops->write(7, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    if ((++pcm_upload_count & 0xFFF) == 0) { // every 4096 bytes
                        printf("[%s] PCM_UPLOAD ... %lu bytes so far\n", chip_name, (unsigned long)pcm_upload_count);
                    }
#endif
                    break;
                case VGMSPI_OP_SEGAPCM_BANK:
                    ops->write(8, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SEGAPCM_BANK shift=%u mask=0x%02X\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_SEGAPCM_ROM_BASE:
                    ops->write(9, reg, data);
#if VGM_SLAVE_VERBOSE_LOG
                    printf("[%s] SEGAPCM_ROM_BASE 0x%02X%02X00\n", chip_name, reg, data);
#endif
                    break;
                case VGMSPI_OP_MUTE:
                    muted = true;
                    printf("[%s] MUTE\n", chip_name);
                    break;
                case VGMSPI_OP_NOP:
                default:
                    break;
            }
        }

        // If draining the FIFO above took a while (e.g. slave_segapcm's
        // one-byte-per-frame ROM upload, which can run for a second or
        // more), `next` is now stale; resync to "now" instead of bursting
        // samples at max speed to catch up to a schedule that's long gone.
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(next, now) > 0) {
            next = now;
            frac_acc = 0;
#if RATE_CHECK_ON
            rate_check_fell_behind++;
#endif
        }

        int16_t sample = ops->render();
        audio_pwm_write(audio_pin, muted ? 0 : sample);

        next = delayed_by_us(next, whole_us);
        frac_acc += rem_us;
        if (frac_acc >= sample_rate_hz) {
            frac_acc -= sample_rate_hz;
            next = delayed_by_us(next, 1);
        }
        busy_wait_until(next);

#if RATE_CHECK_ON
        if (++rate_check_samples >= sample_rate_hz) {
            absolute_time_t rc_now = get_absolute_time();
            int64_t elapsed_us = absolute_time_diff_us(rate_check_start, rc_now);
            printf("[%s] RATE CHECK: %lu samples in %lldus (target %lldus @ %lu Hz), fell_behind=%lu/%lu ticks, reset_rx=%lu\n",
                   chip_name, (unsigned long)rate_check_samples, (long long)elapsed_us,
                   (long long)(1000000LL * rate_check_samples / sample_rate_hz), (unsigned long)sample_rate_hz,
                   (unsigned long)rate_check_fell_behind, (unsigned long)rate_check_samples,
                   (unsigned long)reset_rx);
            rate_check_samples = 0;
            rate_check_fell_behind = 0;
            rate_check_start = rc_now;
        }
#endif
    }
}
