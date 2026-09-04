#include "ymfm_opl.h"
#include <cstdio>
#include <cstdint>
#include <cmath>

class MinimalInterface : public ymfm::ymfm_interface {};

int main() {
    MinimalInterface intf;
    ymfm::ym2413 chip(intf);
    chip.reset();

    auto wr = [&](uint8_t reg, uint8_t data) { chip.write(0, reg); chip.write(1, data); };

    // Instrument 1 (preset), channel 0, mid volume, key on with some F-Num/block.
    wr(0x30, 0x01 << 4 | 0x00); // ch0: instrument=1, volume=0(loudest)
    wr(0x10, 0x69);             // F-Num low
    wr(0x20, 0x10 | 0x01);      // key on, block=0, F-Num hi bit

    int32_t mn = 32767, mx = -32768;
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        ymfm::ym2413::output_data out;
        chip.generate(&out, 1);
        if (i > 2000) {
            if (out.data[0] < mn) mn = out.data[0];
            if (out.data[0] > mx) mx = out.data[0];
        }
    }
    printf("min=%d max=%d sample_rate(3579545)=%u OUTPUTS=%u\n",
           mn, mx, chip.sample_rate(3579545), ymfm::ym2413::OUTPUTS);
    return 0;
}
