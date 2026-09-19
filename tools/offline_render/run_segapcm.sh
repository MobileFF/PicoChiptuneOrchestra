#!/bin/sh
# Build the offline Sega PCM renderer and render a .vgm to _shipped.wav.
# Usage (from repo root):  tools/offline_render/run_segapcm.sh <file.vgm> <out_prefix> [seconds]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/render_segapcm"
cc -O2 -Wall \
  -I "$ROOT/tools/offline_render/shim" -I "$ROOT/tools/host_tests/shim" \
  -I "$ROOT/src/master/src" -I "$ROOT/src/protocol" \
  -I "$ROOT/src/slave_segapcm/src" \
  "$ROOT/tools/offline_render/render_segapcm.c" \
  "$ROOT/src/master/src/vgm_player.c" "$ROOT/src/master/src/vgm_chips.c" \
  "$ROOT/src/slave_segapcm/src/chip_segapcm.c" \
  -lm -o "$OUT"
exec "$OUT" "$@"
