#!/bin/sh
# Build the offline AY+SCC renderer and render a .vgm to _mix/_ay/_scc WAVs.
# Usage (from repo root):  tools/offline_render/run.sh <file.vgm> <out_prefix> [seconds]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/render_wav"
cc -O2 -Wall \
  -I "$ROOT/tools/offline_render/shim" -I "$ROOT/tools/host_tests/shim" \
  -I "$ROOT/src/master/src" -I "$ROOT/src/protocol" \
  -I "$ROOT/src/slave_ay8910/src" -I "$ROOT/src/slave_scc/src" \
  "$ROOT/tools/offline_render/render_wav.c" \
  "$ROOT/src/master/src/vgm_player.c" "$ROOT/src/master/src/vgm_chips.c" \
  "$ROOT/src/slave_ay8910/src/chip_ay8910.c" "$ROOT/src/slave_scc/src/chip_scc.c" \
  -lm -o "$OUT"
exec "$OUT" "$@"
