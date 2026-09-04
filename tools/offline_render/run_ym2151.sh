#!/bin/sh
# Build the offline YM2151 (OPM) renderer and render a .vgm to
# _raw / _shipped / _pwm10 WAVs at the chip's native sample rate.
# Usage (from repo root):
#   tools/offline_render/run_ym2151.sh <file.vgm> <out_prefix> [seconds]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP="${TMPDIR:-/tmp}"
OUT="$TMP/render_ym2151"

INC="-I $ROOT/tools/offline_render/shim -I $ROOT/tools/host_tests/shim \
     -I $ROOT/src/master/src -I $ROOT/src/protocol \
     -I $ROOT/src/slave_ym2151/src -I $ROOT/third_party/ymfm/src"

# C parts (shipped master parser) -- must be compiled as C, not C++.
cc  -O2 -w $INC -c "$ROOT/src/master/src/vgm_player.c" -o "$TMP/ym_vgm_player.o"
cc  -O2 -w $INC -c "$ROOT/src/master/src/vgm_chips.c"  -o "$TMP/ym_vgm_chips.o"

# C++ parts (renderer + shipped YM2151 wrapper + ymfm-OPM).
c++ -O2 -w -std=c++17 $INC -c "$ROOT/tools/offline_render/render_ym2151.cpp"    -o "$TMP/ym_render.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/src/slave_ym2151/src/chip_ym2151.cpp"      -o "$TMP/ym_chip.o"
c++ -O2 -w -std=c++17 $INC -c "$ROOT/third_party/ymfm/src/ymfm_opm.cpp"         -o "$TMP/ym_opm.o"

c++ -O2 "$TMP/ym_vgm_player.o" "$TMP/ym_vgm_chips.o" "$TMP/ym_render.o" \
        "$TMP/ym_chip.o" "$TMP/ym_opm.o" -lm -o "$OUT"

exec "$OUT" "$@"
