// Host-native calibration probe for YM2151 and YM2203, mirroring
// ym2612_probe.cpp / ym2413_probe.cpp: measures each chip's native output
// sample rate, output amplitude range for a simple loud test tone, and a
// rough relative per-second CPU cost (host-only, not RP2040-representative
// in absolute terms, but useful to compare chips against each other and
// against the already-shipped YM2612 slave). See tools/probes/README.md
// for build instructions and docs/design-notes.md for how the results were
// used in chip_ym2151.cpp / chip_ym2203.cpp.
#include "ymfm_opm.h"
#include "ymfm_opn.h"
#include <cstdio>
#include <chrono>

class MinimalInterface : public ymfm::ymfm_interface {};

template <typename Chip>
static void bench(Chip &chip, const char *name, uint32_t sample_rate, int n) {
    using namespace std::chrono;
    typename Chip::output_data out;
    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < n; i++) chip.generate(&out, 1);
    auto t1 = high_resolution_clock::now();
    double us = duration_cast<duration<double, std::micro>>(t1 - t0).count();
    double ns_per_sample = us * 1000.0 / n;
    printf("%s: %.2f ns/sample (host) -> %.2f ms host-CPU per second-of-audio at %u Hz\n",
           name, ns_per_sample, ns_per_sample * sample_rate / 1e6 * 1000.0, sample_rate);
}

int main() {
    MinimalInterface intf1, intf2;

    ymfm::ym2151 opm(intf1);
    opm.reset();
    uint32_t opm_rate = opm.sample_rate(3579545);
    printf("YM2151 sample_rate(3579545) = %u\n", opm_rate);

    ymfm::ym2203 opn(intf2);
    opn.set_fidelity(ymfm::OPN_FIDELITY_MIN);
    opn.reset();
    uint32_t opn_rate = opn.sample_rate(3993600);
    printf("YM2203 (FIDELITY_MIN) sample_rate(3993600) = %u  (OUTPUTS=%u FM=%u SSG=%u)\n",
           opn_rate, ymfm::ym2203::OUTPUTS, ymfm::ym2203::FM_OUTPUTS, ymfm::ym2203::SSG_OUTPUTS);

    // --- amplitude: one loud FM channel (+ one loud SSG channel for OPN) ---
    {
        auto wr = [&](uint8_t reg, uint8_t data) { opm.write(0, reg); opm.write(1, data); };
        wr(0x20, 0xC7); wr(0x28, 0x4C); wr(0x30, 0x00);
        wr(0x60, 0x00); wr(0x68, 0x00); wr(0x70, 0x00); wr(0x78, 0x00);
        wr(0x40, 0x00); wr(0x48, 0x00); wr(0x50, 0x00); wr(0x58, 0x00);
        wr(0x80, 0x1F); wr(0x88, 0x1F); wr(0x90, 0x1F); wr(0x98, 0x1F);
        wr(0x08, 0x78);
        int32_t mn = 32767, mx = -32768;
        for (int i = 0; i < 30000; i++) {
            ymfm::ym2151::output_data out;
            opm.generate(&out, 1);
            if (i > 3000) { if (out.data[0] < mn) mn = out.data[0]; if (out.data[0] > mx) mx = out.data[0]; }
        }
        printf("YM2151 single-channel data[0] min=%d max=%d\n", mn, mx);
    }
    {
        auto wr = [&](uint8_t reg, uint8_t data) { opn.write(0, reg); opn.write(1, data); };
        wr(0x30, 0x71); wr(0x34, 0x0D); wr(0x38, 0x33); wr(0x3C, 0x01);
        wr(0x40, 0x23); wr(0x44, 0x2D); wr(0x48, 0x26); wr(0x4C, 0x00);
        wr(0x50, 0x5F); wr(0x54, 0x99); wr(0x58, 0x5F); wr(0x5C, 0x94);
        wr(0x60, 0x05); wr(0x64, 0x05); wr(0x68, 0x05); wr(0x6C, 0x07);
        wr(0x70, 0x02); wr(0x74, 0x02); wr(0x78, 0x02); wr(0x7C, 0x02);
        wr(0x80, 0x11); wr(0x84, 0x11); wr(0x88, 0x11); wr(0x8C, 0xA6);
        wr(0xB0, 0x07); wr(0xA4, 0x22); wr(0xA0, 0x69); wr(0x28, 0xF0);
        wr(0x00, 0x50); wr(0x01, 0x00); wr(0x07, 0x3E); wr(0x08, 0x0F);
        int32_t mn = 32767, mx = -32768;
        for (int i = 0; i < 40000; i++) {
            ymfm::ym2203::output_data out;
            opn.generate(&out, 1);
            if (i > 4000) {
                int32_t sum = 0;
                for (uint32_t k = 0; k < ymfm::ym2203::OUTPUTS; k++) sum += out.data[k];
                if (sum < mn) mn = sum;
                if (sum > mx) mx = sum;
            }
        }
        printf("YM2203 summed-outputs min=%d max=%d\n", mn, mx);
    }

    bench(opm, "YM2151", opm_rate, 200000);
    bench(opn, "YM2203(MIN)", opn_rate, 200000);

    return 0;
}
