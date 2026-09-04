#include "ymfm_opn.h"
#include <cstdio>
#include <cstdint>
#include <cmath>

class MinimalInterface : public ymfm::ymfm_interface {};

int main() {
    MinimalInterface intf;
    ymfm::ym2612 chip(intf);
    chip.reset();

    auto wr = [&](uint8_t port, uint8_t reg, uint8_t data) {
        if (port == 0) { chip.write(0, reg); chip.write(1, data); }
        else { chip.write(2, reg); chip.write(3, data); }
    };

    // Minimal "beep": channel 0, algorithm 7 (all carriers, simplest additive
    // sound), operator 4 (the only carrier in most algorithms) full volume,
    // fast attack/decay so we get a sustained tone quickly.
    wr(0, 0x30, 0x71); // op1 DT/MUL
    wr(0, 0x34, 0x0D);
    wr(0, 0x38, 0x33);
    wr(0, 0x3C, 0x01);
    wr(0, 0x40, 0x23); // TL (op1) - not the carrier in alg7 but set anyway
    wr(0, 0x44, 0x2D);
    wr(0, 0x48, 0x26);
    wr(0, 0x4C, 0x00); // TL op4 = 0 (loudest) - carrier in algorithm 7
    wr(0, 0x50, 0x5F); wr(0, 0x54, 0x99); wr(0, 0x58, 0x5F); wr(0, 0x5C, 0x94); // RS/AR
    wr(0, 0x60, 0x05); wr(0, 0x64, 0x05); wr(0, 0x68, 0x05); wr(0, 0x6C, 0x07); // AM/D1R
    wr(0, 0x70, 0x02); wr(0, 0x74, 0x02); wr(0, 0x78, 0x02); wr(0, 0x7C, 0x02); // D2R
    wr(0, 0x80, 0x11); wr(0, 0x84, 0x11); wr(0, 0x88, 0x11); wr(0, 0x8C, 0xA6); // D1L/RR
    wr(0, 0xB0, 0x07); // algorithm 7, feedback 0
    wr(0, 0xB4, 0xC0); // pan L+R, no LFO sens

    wr(0, 0xA4, 0x22); // block/fnum hi (channel 0)
    wr(0, 0xA0, 0x69); // fnum lo
    wr(0, 0x28, 0xF0); // key on, channel 0, all operators

    int32_t mn = 32767, mx = -32768;
    long long sumabs = 0;
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        ymfm::ym2612::output_data out;
        chip.generate(&out, 1);
        if (i > 2000) { // let envelope settle past attack
            if (out.data[0] < mn) mn = out.data[0];
            if (out.data[0] > mx) mx = out.data[0];
            sumabs += std::abs((long long)out.data[0]);
        }
    }
    printf("min=%d max=%d avgabs=%lld sample_rate(7670454)=%u\n",
           mn, mx, sumabs / (N - 2000), chip.sample_rate(7670454));
    return 0;
}
