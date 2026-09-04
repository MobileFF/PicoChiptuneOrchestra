# ymfm calibration probes

Host-native (not RP2040) throwaway programs used to determine two things
empirically rather than guess them: each ymfm chip's native output sample
rate (`chip.sample_rate(clock)`) and its output amplitude range, so each FM
slave (`slave_ym2612`, `slave_ym2413`, `slave_ym2151`, `slave_ym2203`) knows
what to scale down to fit `audio_pwm`'s headroom. `ym2151_ym2203_probe.cpp`
also does a rough host-side relative CPU-cost comparison across all four FM
chips (informative only -- host cycles don't map linearly to RP2040
cycles). See `docs/design-notes.md` section 3 and the comments in each
slave's `chip_*.cpp`.

Build and run directly with the host's g++ (no pico-sdk involved):

```sh
cd third_party/ymfm/src
g++ -std=c++17 -O2 -I. ../../../tools/probes/ym2612_probe.cpp \
    ymfm_opn.cpp ymfm_adpcm.cpp ymfm_ssg.cpp -o /tmp/ym2612_probe
/tmp/ym2612_probe

g++ -std=c++17 -O2 -I. ../../../tools/probes/ym2413_probe.cpp \
    ymfm_opl.cpp ymfm_adpcm.cpp ymfm_pcm.cpp -o /tmp/ym2413_probe
/tmp/ym2413_probe

g++ -std=c++17 -O2 -I. ../../../tools/probes/ym2151_ym2203_probe.cpp \
    ymfm_opm.cpp ymfm_opn.cpp ymfm_adpcm.cpp ymfm_ssg.cpp -o /tmp/ym2151_ym2203_probe
/tmp/ym2151_ym2203_probe
```
