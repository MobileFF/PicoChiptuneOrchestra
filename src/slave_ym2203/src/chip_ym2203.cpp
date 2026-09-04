#include "chip_ym2203.h"
#include "ymfm_opn.h"

namespace {

class MinimalInterface : public ymfm::ymfm_interface {};

MinimalInterface s_intf;
ymfm::ym2203 s_chip(s_intf);

// Must stay index-aligned with master/src/vgm_player.c's own copy.
constexpr uint32_t CLOCK_PRESETS[2] = {3993600, 4000000};

} // namespace

extern "C" void ym2203_reset(uint8_t clock_preset) {
    // See chip_ym2612.cpp: ymfm's FM engine has no per-instance clock
    // setting to configure here -- the real-world clock only matters for
    // choosing the correct render sample rate, handled by
    // ym2203_sample_rate_hz() below (which must be called after this, since
    // it depends on the fidelity mode set here).
    (void)clock_preset;
    s_chip.set_fidelity(ymfm::OPN_FIDELITY_MIN); // lowest native sample rate ymfm offers for this chip
    s_chip.reset();
}

extern "C" uint32_t ym2203_sample_rate_hz(uint8_t clock_preset) {
    uint32_t clock = CLOCK_PRESETS[clock_preset < 2 ? clock_preset : 0];
    return s_chip.sample_rate(clock);
}

extern "C" void ym2203_write(uint8_t port, uint8_t reg, uint8_t data) {
    // Only port 0 (VGMSPI_OP_WRITE0) is real. A non-zero port means the SPI
    // resync misdispatched a frame with a dropped opcode byte (e.g. a
    // {WRITE0, reg=0x08, data} -> SCC_KEYON, opcode 0x08); applying it as a
    // register write corrupts state. See chip_ym2151.cpp / design-notes.md.
    if (port != 0) return;
    s_chip.write(0, reg);
    s_chip.write(1, data);
}

extern "C" int16_t ym2203_render(void) {
    ymfm::ym2203::output_data out;
    s_chip.generate(&out, 1);
    // FM (1 output) + SSG (3 outputs) summed. Measured range with one FM
    // channel + one SSG channel active was roughly -8000..+24000 (see
    // tools/probes/ym2151_ym2203_probe.cpp); >>4 leaves headroom for more
    // channels before the clamp below has to do real work.
    int32_t mono = 0;
    for (uint32_t i = 0; i < ymfm::ym2203::OUTPUTS; i++) mono += out.data[i];
    mono >>= 4;
    if (mono > 2047) mono = 2047;
    if (mono < -2048) mono = -2048;
    return (int16_t)mono;
}
