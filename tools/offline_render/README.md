# offline_render — headless VGM→WAV of the shipped emulation + A/B analyzer

Renders a `.vgm` through the **actual shipped** master parser + AY/SCC chip
emulators with perfect timing and none of the real-hardware path (no SPI, no
per-byte CS gap, no inter-core FIFO, no analog mixer), then compares the
result against a reference recording numerically.

Born from the "04 Hot Summer Riding" investigation (`docs/design-notes.md`
§5, memory `project_hot_summer_riding_tempo`): a listener heard the AY part
lagging the SCC around 0:09. Every firmware timing path measured clean (this
rig itself, plus the master's [LAG] trace and both slaves' RATE CHECK on
real hardware) -- tempo/timing are exact. The still-unresolved suspicion is
that the AY envelope's instant-step re-attack sounds harder than real
hardware in a busy passage; three per-channel-level smoothing attempts
(`AY8910_LEVEL_SLEW_HZ`, since replaced by a post-mix filter,
`AY8910_OUTPUT_LPF_HZ` -- see chip_ay8910.c) made no confirmed difference on
real hardware, so this remains open.

## render_wav.c / run.sh

```sh
tools/offline_render/run.sh "調査用/04 Hot Summer Riding.vgm" /tmp/hr 40
#   -> /tmp/hr_mix.wav /tmp/hr_ay.wav /tmp/hr_scc.wav   (44100 mono s16, peak-normalised)
# optional 4th/5th args: ay_lpf_hz scc_lpf_hz  (one-pole LPF on that stem; experiment knob)
```

`sleep_until()` (from `shim/pico/stdlib.h`) is implemented as the sample
clock: every `wait_samples()` in `vgm_player.c` renders exactly that many
samples through `ay8910_render()` / `scc_render()`.

To sweep the AY output filter / SCC volume slew hooks, build directly with
the defines:

```sh
cc -O2 -w -I tools/offline_render/shim -I tools/host_tests/shim \
   -I src/master/src -I src/protocol -I src/slave_ay8910/src -I src/slave_scc/src \
   -DAY8910_OUTPUT_LPF_HZ=3000 -DSCC_VOLUME_SLEW_HZ=0 \
   tools/offline_render/render_wav.c src/master/src/vgm_player.c src/master/src/vgm_chips.c \
   src/slave_ay8910/src/chip_ay8910.c src/slave_scc/src/chip_scc.c -lm -o /tmp/render_slew
```

Only AY + SCC are wired (`slave_bus_send` / `slave_bus_write` in
`render_wav.c`); add cases for other chips as needed.

## render_ym2151.cpp / run_ym2151.sh  (YM2151 / OPM, shipped ymfm core)

```sh
tools/offline_render/run_ym2151.sh "調査用/03 Splash Wave.vgm" /tmp/splash 90
#   -> /tmp/splash_raw.wav      (L+R straight from ymfm, pre-scale)
#      /tmp/splash_shipped.wav  (after chip_ym2151.cpp >>6 + clamp = 12-bit)
#      /tmp/splash_pwm10.wav    (after a further >>2 = what the 10-bit PWM DAC reproduces)
```

Renders through the *shipped* `vgm_player.c` + `src/slave_ym2151/src/chip_ym2151.cpp`
+ `third_party/ymfm`, at the YM2151's native rate for the file's clock
(`ym2151_sample_rate_hz()` -- 62500 Hz at 4 MHz). Uses `ym2151_render_raw()`
(added to the wrapper for exactly this). Each WAV is peak-normalised so the
three are level-matched for ear A/B; the run also prints raw peak / RMS /
effective bits and how often `>>6` clamps.

Localises grit/distortion: `_raw` clean but `_pwm10` gritty => it's our
output scaling + 10-bit PWM, not the emulator. `_raw` itself distorted =>
emulator core or our register feed -- then A/B against Nuked-OPM below.

## render_ym2151_nuked.c / run_ym2151_nuked.sh  (reference OPM core)

Same as above but synthesises with **Nuked-OPM** (cycle-accurate, the de
facto reference). Needs `vendor/nuked-opm/` (LGPL-2.1, git-ignored,
host-only -- fetch per `vendor/README.md`).

```sh
tools/offline_render/run_ym2151_nuked.sh "調査用/03 Splash Wave.vgm" /tmp/splash 90
#   -> /tmp/splash_nuked_raw.wav  /tmp/splash_nuked_shipped.wav
```

Then `analyze.py /tmp/splash_raw.wav /tmp/splash_nuked_raw.wav` (same sample
rate, so onset-lag + spectral diff line up). Divergence on specific notes =>
ymfm-OPM or our usage of it is the culprit. Cycle-accurate, so ~50-100x
slower than the ymfm render -- a 90 s render takes a few minutes.

**Caveat:** the raw stem-sum `_mix.wav` is AY-heavy (centroid ~200 Hz vs a
real recording's ~550 Hz). Trust its onset / timing metrics, not its
spectral balance.

## render_segapcm.c / run_segapcm.sh  (Sega PCM, shipped software emulator)

Same idea as render_wav.c / render_ym2151.cpp, for the Sega PCM path: links
the *shipped* master parser (`src/master/src/vgm_player.c`) and the *shipped*
`src/slave_segapcm/src/chip_segapcm.c`, with no SPI, no per-byte CS gap/burst,
no inter-core FIFO, no PWM. Born from the Space Harrier / `02 Theme.vgm`
"extra drum-like hits throughout the song" investigation (2026-09-19, see
`docs/design-notes.md`'s Sega PCM section): this is what proved
`chip_segapcm.c`'s own register-driven logic was innocent (clean output, no
clipping) and pointed at the ROM upload's missing re-anchor instead (fixed in
`vgm_player.c`).

```sh
tools/offline_render/run_segapcm.sh "調査用/02 Theme.vgm" /tmp/spct 230
#   -> /tmp/spct_shipped.wav   (segapcm_render()'s own output -- already
#                               mixed + >>4 + clamped, exactly what real
#                               hardware would play given perfect delivery)
```

Renders at the chip's own native rate for the song's clock preset
(`segapcm_sample_rate_hz()` -- 31250 Hz at 4 MHz, 62500 Hz at 8 MHz). Prints
peak/RMS. If the artifact this tool was built to chase reappears here, it's a
bug in `chip_segapcm.c` itself, deterministic and independent of the SPI
transport; if it's clean here but not on real hardware, look at the transport
(SPI timing, the slave's inter-core FIFO, the ROM upload path) instead --
exactly what happened in the investigation above.

## analyze.py  (numpy only; PCM-16 WAV in only — convert with Audacity)

```sh
python3 tools/offline_render/analyze.py A.wav [B.wav] \
        [--win LO HI] [--suspect 9.6] [--label A B]
```

Per file: spectral-flux onset times, median inter-onset-interval → tempo,
onset grid around `--suspect`. With two files: sliding-window
cross-correlation lag(t) in ms (constant lag = locked; a jump = a real
desync there).

For a full A/B against a reference recording, the ad-hoc scripts used in the
investigation also did: autocorrelation tempo, leading-silence trim + global
fine offset, per-third offset (drift check), onset-count ratio at several
thresholds, and average-spectrum band energies + centroid.
