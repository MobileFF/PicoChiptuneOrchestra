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
// here once, then only sends compact DAC_START/RATE/STOP frames; this code
// advances the read cursor every output sample and writes each byte to the
// chip's DAC data register (port 0, reg 0x2A), exactly as the game's own
// timer ISR would on real hardware. See VGMSPI_OP_YM2612_DAC_* / ports 20-25
// in slave_engine.c. DAC mode itself is enabled by the VGM's own
// `0x52 2B 80` write, forwarded as a normal port-0 register write.
uint8_t  s_pcm[YM2612_PCM_BUDGET];
uint32_t s_pcm_wr;      // upload write cursor
uint32_t s_pcm_len;     // valid bank length (== s_pcm_wr after the burst)
uint32_t s_dac_cursor;  // DAC read cursor
uint8_t  s_dac_seek_hi; // latched offset[23:16] for the next DAC_START
bool     s_dac_active;
uint32_t s_dac_rate16;  // bank bytes per output sample * 65536 (1..65535)
uint32_t s_dac_frac;    // fractional accumulator, same 16.16 scale

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
    s_dac_cursor = 0;
    s_dac_seek_hi = 0;
    s_dac_active = false;
    s_dac_rate16 = 0;
    s_dac_frac = 0;
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
    } else if (port == 20) {          // PCM bank upload-cursor reset
        s_pcm_wr = 0;
        s_pcm_len = 0;
    } else if (port == 21) {          // upload one PCM bank byte
        if (s_pcm_wr < YM2612_PCM_BUDGET) {
            s_pcm[s_pcm_wr++] = data;
            s_pcm_len = s_pcm_wr;
        }
    } else if (port == 22) {          // DAC_START: reg:data = read offset [15:0]
        s_dac_cursor = ((uint32_t)s_dac_seek_hi << 16) | ((uint32_t)reg << 8) | data;
        s_dac_active = true;
        s_dac_frac = 0;
    } else if (port == 23) {          // DAC_RATE: reg:data = bytes/sample * 65536
        s_dac_rate16 = ((uint32_t)reg << 8) | data;
    } else if (port == 24) {          // DAC_STOP
        s_dac_active = false;
    } else if (port == 25) {          // DAC_SEEK: data = read offset [23:16]
        s_dac_seek_hi = data;
    }
}

extern "C" int16_t ym2612_render(void) {
    // Feed the DAC from the uploaded bank at the master-set rate. The MD DAC
    // rate is always below the 44.1 kHz output rate, so this advances 0 or 1
    // bytes per output sample -- the while() is just belt-and-braces.
    if (s_dac_active && s_dac_rate16) {
        s_dac_frac += s_dac_rate16;
        while (s_dac_frac >= 65536u) {
            s_dac_frac -= 65536u;
            uint8_t s = (s_dac_cursor < s_pcm_len) ? s_pcm[s_dac_cursor] : 0x80u;
            s_dac_cursor++;
            s_chip.write(0, 0x2A); // YM2612 port 0, DAC data register
            s_chip.write(1, s);
        }
    }
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
