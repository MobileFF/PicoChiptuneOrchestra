#include "chip_ym2413.h"
#include "ymfm_opl.h"

namespace {

class MinimalInterface : public ymfm::ymfm_interface {};

MinimalInterface s_intf;
ymfm::ym2413 s_chip(s_intf); // nullptr instrument_data -> built-in preset ROM table

// Must stay index-aligned with master/src/vgm_player.c's own copy.
constexpr uint32_t CLOCK_PRESETS[2] = {3579545, 3546893};

} // namespace

extern "C" void ym2413_reset(uint8_t clock_preset) {
    // See chip_ym2151.cpp: ymfm's FM engine has no per-instance clock
    // setting to configure here -- the real-world clock only matters for
    // choosing the correct render sample rate, handled by
    // ym2413_sample_rate_hz() below.
    (void)clock_preset;
    s_chip.reset();
}

extern "C" uint32_t ym2413_sample_rate_hz(uint8_t clock_preset) {
    uint32_t clock = CLOCK_PRESETS[clock_preset < 2 ? clock_preset : 0];
    return s_chip.sample_rate(clock);
}

extern "C" void ym2413_write(uint8_t port, uint8_t reg, uint8_t data) {
    // Only port 0 (VGMSPI_OP_WRITE0) is real. A non-zero port means the SPI
    // resync misdispatched a frame with a dropped opcode byte; applying it
    // corrupts state. See chip_ym2151.cpp / design-notes.md.
    if (port != 0) return;
    s_chip.write(0, reg);
    s_chip.write(1, data);
}

extern "C" int16_t ym2413_render(void) {
    ymfm::ym2413::output_data out;
    s_chip.generate(&out, 1);
    // Measured range for one loud channel is 0..2560 (see
    // tools/probes/ym2413_probe.cpp) -- the real YM2413's cost-reduced DAC is
    // documented to produce an asymmetric, mostly-unipolar waveform, so this
    // isn't a bug.
    //
    // FIX (2026-09-05): >>1 was not remotely enough headroom despite the
    // comment's claim. YM2413 has 9 melody channels, and a probe with 9
    // simultaneous loud channels (tools/probes/ym2413_chord_probe.cpp)
    // measured a summed peak of ~21000 -- >>1 of that
    // is ~10500, twelve times over the +-2047 clamp below, i.e. hard clipping
    // ("音割れ") on any full chord, not just rare fortissimo peaks. Reported
    // as intermittent distortion on real hardware (worse in fuller chords,
    // fine on sparse single-note passages -- consistent with this). Changed
    // to >>4, matching slave_ym2203's shift (the other many-channel FM chip
    // here): 21000>>4 ~= 1310, safely under the clamp with headroom to spare.
    // Confirmed on real hardware (2026-09-05): clipping gone, no report of
    // it now sounding too quiet against the other chips.
    // Quieter overall than before by construction -- if this now sounds too
    // quiet next to the other chips in the analog mix, re-tune per
    // docs/circuit.md's mixing resistors rather than un-doing this shift.
    int32_t mono = (int32_t)out.data[0] >> 4;
    if (mono > 2047) mono = 2047;
    if (mono < -2048) mono = -2048;
    return (int16_t)mono;
}
