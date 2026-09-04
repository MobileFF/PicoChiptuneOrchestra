#include "chip_ym2151.h"
#include "ymfm_opm.h"

namespace {

class MinimalInterface : public ymfm::ymfm_interface {};

MinimalInterface s_intf;
ymfm::ym2151 s_chip(s_intf);

// Real-world YM2151 clocks seen across systems that used this chip: index 0
// (most common, e.g. many PC-88/PC-98/X68000-adjacent and arcade uses) and
// index 1 (also very common, e.g. several Sega and Capcom arcade boards).
// Must stay index-aligned with master/src/vgm_player.c's own copy of this
// table.
constexpr uint32_t CLOCK_PRESETS[2] = {3579545, 4000000};

} // namespace

extern "C" void ym2151_reset(uint8_t clock_preset) {
    // ymfm's FM engine has no per-instance clock setting to configure here:
    // register values (KC/KF) already encode pitch relative to a fixed
    // internal ratio, and the real-world clock only matters for choosing
    // the correct RENDER sample rate (see ym2151_sample_rate_hz below) --
    // ymfm's own sample_rate(clock) is exactly that translation. This
    // reset() call is genuinely clock-independent, not "clock doesn't
    // matter here" -- see chip_ops_t.sample_rate_hz's doc comment for the
    // distinction (this repo used to conflate the two).
    (void)clock_preset;
    s_chip.reset();
}

extern "C" uint32_t ym2151_sample_rate_hz(uint8_t clock_preset) {
    uint32_t clock = CLOCK_PRESETS[clock_preset < 2 ? clock_preset : 0];
    return s_chip.sample_rate(clock);
}

extern "C" void ym2151_write(uint8_t port, uint8_t reg, uint8_t data) {
    // Only VGMSPI_OP_WRITE0 (port 0) is a real YM2151 write. A non-zero port
    // reaches here only when slave_spi_rx.c's resync misdispatches a frame
    // whose real opcode byte was dropped -- classically a {WRITE0, reg=0x08,
    // data} key-on frame decoded as VGMSPI_OP_SCC_KEYON (opcode value 0x08)
    // -> port 5. Applying it as a register write corrupts the FM patch
    // (audible as distortion on key-on-dense music, e.g. OutRun); dropping
    // it costs at most one lost write. See docs/design-notes.md.
    if (port != 0) return;
    s_chip.write(0, reg);
    s_chip.write(1, data);
}

// Raw per-side ymfm OPM output for one sample, BEFORE this wrapper's
// down-scale/clamp. Exposed for tools/offline_render (A/B of the emulator
// core against the shipped >>6 path and against a reference OPM). Advances
// the chip one sample -- call this OR ym2151_render() once per output
// sample, never both.
extern "C" void ym2151_render_raw(int32_t *l, int32_t *r) {
    ymfm::ym2151::output_data out;
    s_chip.generate(&out, 1);
    *l = out.data[0];
    *r = out.data[1];
}

extern "C" int16_t ym2151_render(void) {
    int32_t l, r;
    ym2151_render_raw(&l, &r);
    // ymfm scales OPM output much hotter than OPN2/YM2612 -- a single loud
    // channel measured +-32700 per side (see ym2151_ym2203_probe.cpp), so
    // (L+R) for one loud channel ~= +-65000.
    //
    // >>7 was chosen for clamp headroom, but it leaves QUIET tracks tiny:
    // OutRun's "Splash Wave" peaked at only 294/2048 (host render) = ~6
    // effective bits into audio_pwm's 10-bit PWM -- audible as timbre
    // "grit"/distortion (quantisation + PWM-carrier bleed on a near-silent
    // duty), worst on sustained FM tones. YM2203 doesn't show this because
    // its output sits much higher. >>6 doubles the level (one loud OPM
    // channel -> ~1020, still room for ~2 before the clamp) and roughly
    // halves that quantisation floor. See docs/design-notes.md §5.
    int32_t mono = (l + r) >> 6;
    if (mono > 2047) mono = 2047;
    if (mono < -2048) mono = -2048;
    return (int16_t)mono;
}
