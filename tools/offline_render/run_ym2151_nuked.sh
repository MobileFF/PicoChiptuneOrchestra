#!/bin/sh
# Build the offline YM2151 renderer that uses Nuked-OPM (reference core) and
# render a .vgm to _nuked_raw / _nuked_shipped WAVs at the chip's native rate.
# Pairs with run_ym2151.sh (shipped ymfm core) for an A/B.
#
# Needs tools/offline_render/vendor/nuked-opm/ (git-ignored, LGPL, host-only) --
# fetch it per tools/offline_render/vendor/README.md if absent.
#
# Usage (from repo root):
#   tools/offline_render/run_ym2151_nuked.sh <file.vgm> <out_prefix> [seconds]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${TMPDIR:-/tmp}/render_ym2151_nuked"
V="$ROOT/tools/offline_render/vendor/nuked-opm"

if [ ! -f "$V/opm.c" ]; then
    echo "error: $V/opm.c missing -- see tools/offline_render/vendor/README.md" >&2
    exit 1
fi

cc -O2 -w \
  -I "$ROOT/tools/offline_render/shim" -I "$ROOT/tools/host_tests/shim" \
  -I "$ROOT/src/master/src" -I "$ROOT/src/protocol" -I "$V" \
  "$ROOT/tools/offline_render/render_ym2151_nuked.c" \
  "$ROOT/src/master/src/vgm_player.c" "$ROOT/src/master/src/vgm_chips.c" \
  "$V/opm.c" \
  -lm -o "$OUT"

exec "$OUT" "$@"
