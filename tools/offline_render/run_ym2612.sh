#!/bin/sh
# Build the offline YM2612 (OPN2 / Mega Drive FM+DAC) renderer and render a
# .vgm to _raw / _shipped WAVs at the chip's native sample rate.
# Usage (from repo root):
#   tools/offline_render/run_ym2612.sh <file.vgm> <out_prefix> [seconds]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP="${TMPDIR:-/tmp}"
OUT="$TMP/render_ym2612"

INC="-I $ROOT/tools/offline_render/shim -I $ROOT/tools/host_tests/shim \
     -I $ROOT/src/master/src -I $ROOT/src/protocol \
     -I $ROOT/src/slave_ym2612/src -I $ROOT/third_party/ymfm/src"

# C parts (shipped master parser) -- must be compiled as C, not C++.
cc  -O2 -w $INC -c "$ROOT/src/master/src/vgm_player.c" -o "$TMP/y2_vgm_player.o"
cc  -O2 -w $INC -c "$ROOT/src/master/src/vgm_chips.c"  -o "$TMP/y2_vgm_chips.o"

# C++ parts (renderer + shipped YM2612 wrapper + ymfm-OPN2). Matches
# src/slave_ym2612/CMakeLists.txt's ymfm file list (OPN2 needs its own SSG
# and ADPCM units built in even though this wrapper's register writes never
# touch them).
c++ -O2 -w -std=c++17 $INC -c "$ROOT/tools/offline_render/render_ym2612.cpp" -o "$TMP/y2_render.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/src/slave_ym2612/src/chip_ym2612.cpp"   -o "$TMP/y2_chip.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/third_party/ymfm/src/ymfm_opn.cpp"     -o "$TMP/y2_opn.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/third_party/ymfm/src/ymfm_adpcm.cpp"   -o "$TMP/y2_adpcm.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/third_party/ymfm/src/ymfm_ssg.cpp"     -o "$TMP/y2_ssg.o"

c++ -O2 "$TMP/y2_vgm_player.o" "$TMP/y2_vgm_chips.o" "$TMP/y2_render.o" \
        "$TMP/y2_chip.o" "$TMP/y2_opn.o" "$TMP/y2_adpcm.o" "$TMP/y2_ssg.o" \
        -lm -o "$OUT"

exec "$OUT" "$@"
