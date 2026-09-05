// Companion to ym2413_probe.cpp: that one measures a single loud channel
// (0..2560); this one keys on all 9 melody channels at once, loudest preset
// instrument, to measure the actual worst case chip_ym2413.cpp's final
// >>N shift has to survive. Used to size that shift correctly -- see the
// "FIX (2026-09-05)" comment in src/slave_ym2413/src/chip_ym2413.cpp: the
// original >>1 was sized off the single-channel probe alone and clipped hard
// ("音割れ") on anything fuller than ~1.5 simultaneous loud channels.
#include "ymfm_opl.h"
#include <cstdio>
#include <cstdint>

class MinimalInterface : public ymfm::ymfm_interface {};

int main() {
    MinimalInterface intf;
    ymfm::ym2413 chip(intf);
    chip.reset();
    auto wr = [&](uint8_t reg, uint8_t data) { chip.write(0, reg); chip.write(1, data); };

    // Instrument 1 (preset), loudest volume, on channels 0..8 (all 9 melody
    // channels), each a slightly different note so they don't perfectly
    // phase-cancel -- a worst-case-ish full chord.
    for (int ch = 0; ch < 9; ch++) {
        wr(0x30 + ch, (0x01 << 4) | 0x00);            // instrument=1, volume=loudest
        wr(0x10 + ch, 0x50 + ch * 8);                 // F-Num low, vary per channel
        wr(0x20 + ch, 0x10 | 0x01 | ((ch & 1) << 1)); // key on, block varies a bit
    }

    int32_t mn = 32767, mx = -32768;
    int64_t sum_abs = 0;
    const int N = 40000;
    for (int i = 0; i < N; i++) {
        ymfm::ym2413::output_data out;
        chip.generate(&out, 1);
        if (i > 4000) { // skip the attack transient
            if (out.data[0] < mn) mn = out.data[0];
            if (out.data[0] > mx) mx = out.data[0];
            sum_abs += (out.data[0] < 0 ? -out.data[0] : out.data[0]);
        }
    }
    printf("9ch loud chord: min=%d max=%d avg_abs=%lld\n",
           mn, mx, (long long)(sum_abs / (N - 4000)));
    return 0;
}
