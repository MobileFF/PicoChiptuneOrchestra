cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(vgmplay_multi_mcu C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(VGM_SLAVE_VERBOSE_LOG
    "Log every SPI frame each slave receives, over UART/USB CDC. Bring-up \
debugging only: this WILL glitch real-time audio (see docs/design-notes.md \
\"ログの確認方法\"). Leave OFF for normal listening use." OFF)

option(VGM_SLAVE_RATE_CHECK
    "Emit just the once-per-second '[chip] RATE CHECK: N samples in Xus ...' \
line (no per-write spam), so the numbers reflect the real render() cost. \
Use this, not VGM_SLAVE_VERBOSE_LOG, to check whether a slave keeps up with \
44.1kHz. Safe-ish for listening (1 short print/sec)." OFF)

option(VGM_SLAVE_RX_TRACE
    "Capture the first ~900 raw SPI bytes a slave receives (value + timestamp) \
into a RAM ring buffer on core0 and dump them once, WITHOUT the per-frame \
core1 printf that VGM_SLAVE_VERBOSE_LOG also enables. That per-frame printf \
can itself back-pressure core0 into overflowing the SPI RX FIFO, so a \
VGM_SLAVE_VERBOSE_LOG trace can show byte loss the logging caused; this \
option's trace reflects what the wire actually delivered. Bring-up debugging \
only." OFF)

option(VGM_MASTER_VERBOSE_LOG
    "Log every VGM command the master parses and every SPI frame it sends \
in response, over UART/USB CDC. Bring-up debugging only: printing this much \
can perturb the master's wait_samples() timing (see docs/design-notes.md \
\"ログの確認方法\"). Leave OFF for normal listening use." OFF)

option(VGM_MASTER_DEBUG_TRACE
    "Master timing-diagnosis logs: [FIRST] first-write time of each voice, \
[EV] windowed AY R13/EP + SCC key-on trace, [LAG] behind-schedule spikes, \
plus a 10 s boot countdown so a USB terminal can attach first. A few short \
prints -- lighter than VGM_MASTER_VERBOSE_LOG -- but still OFF for normal \
listening use. ([TIME] MM:SS every 2 s stays on regardless.)" OFF)

# RP2350 / Pico 2 overclock for the compute-heavy ymfm slaves. Applied via
# slave_common/include/slave_overclock.h (set_sys_clock_khz + optional vreg
# bump + clk_peri re-pinned to 48 MHz so SPI/UART stay in spec). No effect on
# RP2040 builds.
#
# VGM_RP2350_SYSCLK_KHZ / VGM_RP2350_VREG_MV apply to YM2151 / YM2612 /
# SegaPCM. 250 MHz / SDK-default 1.10 V is the confirmed-stable baseline
# (YM2151 real-time, hardware-verified).
set(VGM_RP2350_SYSCLK_KHZ 250000 CACHE STRING
    "set_sys_clock_khz value for the YM2151/YM2612/SegaPCM RP2350 builds")
set(VGM_RP2350_VREG_MV 0 CACHE STRING
    "core voltage (mV) for the YM2151/YM2612/SegaPCM RP2350 builds; 0 = SDK default 1.10 V")

# YM2203 alone renders at ~166 kHz (3x the others) and needs a bigger bump:
# 320 MHz + 1.15 V is hardware-confirmed on one board (250/300 MHz played
# flat). Past ~300 MHz the RP2350 needs ~1150-1250 mV to be stable at all
# (default 1.10 V = no audio). Clamped to the SDK-"safe" max 1300.
set(VGM_YM2203_SYSCLK_KHZ 320000 CACHE STRING
    "set_sys_clock_khz value for the YM2203 RP2350 build")
set(VGM_YM2203_VREG_MV 1150 CACHE STRING
    "core voltage (mV) for the YM2203 RP2350 build")

# YM2151 renders at 62500 Hz for a 4 MHz clock (OutRun); at 250 MHz the core
# can't sustain that on FM-dense material -- RATE CHECK showed fell_behind up
# to ~20000/62500 and playback 1-5.5% slow (a wandering ~0-90 cent flat).
# Same treatment as YM2203: 320 MHz + 1.15 V, hardware-verified. clk_peri is
# still pinned to 48 MHz in slave_overclock.h so the SPI slave keeps decoding.
set(VGM_YM2151_SYSCLK_KHZ 320000 CACHE STRING
    "set_sys_clock_khz value for the YM2151 RP2350 build")
set(VGM_YM2151_VREG_MV 1150 CACHE STRING
    "core voltage (mV) for the YM2151 RP2350 build")

# RP2040 (plain Pico / RP2040-Zero) slave overclock. 0 = SDK default clock.
# Set e.g. 250000 to give the AY-3-8910 / SCC slaves enough headroom that
# slave_engine never hits its silent "fell_behind" resync on write-dense
# songs. clk_peri is pinned to 48 MHz in slave_overclock.h so SPI still
# decodes. Applies to slave_ay8910 / slave_scc.
set(VGM_RP2040_SYSCLK_KHZ 0 CACHE STRING
    "set_sys_clock_khz value for the RP2040 AY-3-8910/SCC slave builds; 0 = SDK default")

# One-pole smoothing of each channel's output level (AY envelope/register
# level, SCC volume register) instead of the instant per-sample step the
# emulators otherwise apply. From the "Hot Summer Riding" investigation
# (tools/offline_render/, docs/design-notes.md): a software A/B against a
# reference recording found the instant-step output attacked harder (~1.5x
# the onset density in a busy passage) than the real track, heard as the AY
# melody articulating late against the other chip. ~5-15 Hz roughly halves
# that gap; 10 Hz on real hardware removed the perceived AY lag entirely
# (user-confirmed 2026-09-02) with no downside on the AY.
#
# AY defaults ON at 10 Hz. SCC defaults OFF (0): its output already matched
# the reference, and 10 Hz there slightly altered the SCC timbre away from
# the original (user-confirmed). 0 = byte-identical to the pre-slew emulator.
set(VGM_AY8910_LEVEL_SLEW_HZ 10 CACHE STRING
    "one-pole smoothing (Hz) of AY-3-8910 per-channel output level; 0 = off/instant")
set(VGM_SCC_VOLUME_SLEW_HZ 0 CACHE STRING
    "one-pole smoothing (Hz) of SCC per-channel volume; 0 = off/instant (default)")

pico_sdk_init()

# Firmware modules live under src/ (protocol/ is header-only, pulled in via
# each module's include paths). third_party/ and tools/ stay at the root.
add_subdirectory(src/slave_common)
add_subdirectory(src/slave_sn76489)
add_subdirectory(src/slave_ay8910)
add_subdirectory(src/slave_ym2612)
add_subdirectory(src/slave_ym2413)
add_subdirectory(src/slave_ym2151)
add_subdirectory(src/slave_ym2203)
add_subdirectory(src/slave_scc)
add_subdirectory(src/slave_segapcm)
add_subdirectory(src/master)
