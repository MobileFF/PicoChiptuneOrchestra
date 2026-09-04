#include "chip_ym2612.h"
#include "ymfm_opn.h"

namespace {

// Default virtual methods (no ADPCM external memory, no timer/IRQ
// consumers) are sufficient: this slave only needs FM register writes in
// and PCM samples out.
class MinimalInterface : public ymfm::ymfm_interface {};

MinimalInterface s_intf;
ymfm::ym2612 s_chip(s_intf);

// Must stay index-aligned with master/src/vgm_player.c's own copy.
constexpr uint32_t CLOCK_PRESETS[2] = {7670454, 7600489}; // NTSC, PAL

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
    }
}

extern "C" int16_t ym2612_render(void) {
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
