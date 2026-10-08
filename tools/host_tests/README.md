# Host-side tests for the master firmware

`src/master/src/vgm_player.c` and `vgz_inflate.c` only depend on FatFs (`ff.h`)
and a handful of `pico/stdlib.h` timing calls, not on real hardware. `shim/`
provides minimal stdio-backed stand-ins for both, so the *actual* shipped
source files can be compiled and run natively on the host -- no RP2040
involved. `stub_slave_bus.c` replaces `src/master/src/slave_bus.c` (which needs
real SPI/GPIO) with one that just logs what it would have sent.

This is how `vgm_player.c`'s VGM header parsing, command dispatch, unknown-
command byte-skipping, data-block skipping, and loop-point seeking were
verified without access to real hardware.

## VGM parsing + dispatch

```sh
MASTER_SRC=../../src/master/src
PROTO=../../src/protocol
gcc -O0 -g -Wall -I shim -I "$MASTER_SRC" -I "$PROTO" \
    test_vgm_player.c "$MASTER_SRC/vgm_player.c" "$MASTER_SRC/vgm_chips.c" stub_slave_bus.c \
    -o /tmp/vgm_player_hosttest
/tmp/vgm_player_hosttest path/to/some.vgm   # prints every RESET/WRITE/MUTE dispatched
```

## Loop-point handling

```sh
gcc -O0 -g -Wall -I shim -I "$MASTER_SRC" -I "$PROTO" \
    test_vgm_loop.c "$MASTER_SRC/vgm_player.c" "$MASTER_SRC/vgm_chips.c" stub_slave_bus.c \
    -o /tmp/vgm_loop_hosttest
/tmp/vgm_loop_hosttest path/to/looping.vgm
```

## Sega PCM ROM-image data block (0x67 type 0x80) + bank config

Builds its own recording slave_bus and synthetic VGMs, then asserts the
8-byte block prefix is stripped, an `UPLOAD_RESET` seeks to the block's
start address, exactly `size - 8` bytes are streamed, the parser stays in
sync, and the VGM header 0x3C interface register is decoded and forwarded
once as `SEGAPCM_BANK` (checked for intf=0 and a non-zero value).
Self-contained -- takes no argument.

```sh
gcc -O0 -g -Wall -I shim -I "$MASTER_SRC" -I "$PROTO" \
    test_vgm_segapcm_block.c "$MASTER_SRC/vgm_player.c" "$MASTER_SRC/vgm_chips.c" \
    -o /tmp/segapcm_block_hosttest
/tmp/segapcm_block_hosttest   # prints "ok" and exits 0, or FAIL lines and exits 1
```

## YM2612 PCM / DAC streaming (Mega Drive "PCM": checkpoint protocol)

Builds its own recording slave_bus and a synthetic MD VGM, then asserts the
type-0 PCM data block is uploaded byte-for-byte as `YM2612_PCM_BYTE` (with a
`YM2612_PCM_RESET` re-anchor -- absolute cursor >> 2 -- at song start and
every 128 bytes during the upload), a run of `0x8n` commands emits
`DAC_SEEK`+`DAC_START` (absolute position) and `DAC_RATE` (a rate seed from
the run's first `0x8n` wait) at the run start, one `DAC_SYNC` checkpoint
(position >> 2) every `DAC_SYNC_EVERY` (32) `0x8n` commands -- NOT one frame
per `0x8n` -- a `0xE0` seek re-anchors the next run (with `DAC_SEEK` for the
>64 KB offset high byte), the `0x52 2B 80` DAC-enable write is still
forwarded normally, and the parser stays in sync. Self-contained -- takes no
argument. See `docs/design-notes.md`'s YM2612 DAC/PCM section for why this
replaced an earlier one-frame-per-`0x8n` design (SPI bandwidth, click/
decimation, and boot-race failure modes it couldn't avoid).

```sh
gcc -O0 -g -Wall -I shim -I "$MASTER_SRC" -I "$PROTO" \
    test_vgm_ym2612_dac.c "$MASTER_SRC/vgm_player.c" "$MASTER_SRC/vgm_chips.c" \
    -o /tmp/ym2612_dac_hosttest
/tmp/ym2612_dac_hosttest   # prints "ok" and exits 0, or FAIL lines and exits 1
```

## Streaming gzip (.vgz) decompression

Exercises the same 32KB-window streaming loop as `vgz_inflate.c` (with
`f_read`/`f_write`/`f_lseek` swapped for `fread`/`fwrite`/`fseek`) against a
real `.gz` file, and confirms the output is byte-identical to the original:

```sh
gcc -O2 -I ../../third_party/miniz_tinfl \
    test_vgz_inflate.c ../../third_party/miniz_tinfl/miniz_tinfl.c \
    -o /tmp/vgz_inflate_hosttest
gzip -k -f some_file
/tmp/vgz_inflate_hosttest some_file.gz /tmp/out
cmp some_file /tmp/out && echo MATCH
```

## Content-based gzip detection (`vgz_looks_like_gzip()`)

Links the *actual shipped* `vgz_inflate.c`. Asserts the 2-byte gzip magic
(0x1F 0x8B) sniff is right regardless of what the rest of the file holds
(or whether it exists at all) -- this is what `main.c`'s `play_one()` uses
to decide whether to decompress a file BY CONTENT rather than by its
`.vgm`/`.vgz` extension, after a real-world VGM pack
(`調査用/Ashura-SMS/*.vgm`) turned up gzip-compressed data shipped under a
plain `.vgm` name; read raw (extension said not to decompress), it failed
the "Vgm " magic check and was silently skipped every time. Self-contained
-- takes no argument.

```sh
gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../third_party/miniz_tinfl \
    test_vgz_sniff.c ../../src/master/src/vgz_inflate.c \
    ../../src/master/src/inflate_scratch.c \
    ../../third_party/miniz_tinfl/miniz_tinfl.c -o /tmp/vgz_sniff_test
/tmp/vgz_sniff_test   # prints "ok" and exits 0, or FAIL lines and exits 1
```

(`inflate_scratch.c` is the shared tinfl scratch buffer vgz_inflate.c and
cover_image.c's PNG decoding both use -- see inflate_scratch.h.)

## Sega PCM chip core

`src/slave_segapcm/src/chip_segapcm.c` has no ymfm dependency, so it builds and
runs directly on the host with no shims needed at all:

```sh
SEGA=../../src/slave_segapcm/src
gcc -O0 -g -Wall -I "$SEGA" test_segapcm_render.c "$SEGA/chip_segapcm.c" \
    -o /tmp/segapcm_render_test
/tmp/segapcm_render_test   # prints "ok" and exits 0, or prints FAIL lines and exits 1
```

## SD-card config parser (vgmplay.ini)

`src/master/src/player_config.c`'s INI parser (`player_config_apply`) is pure
string handling; `test_player_config.c` stubs the four `slave_bus_set_*`
sinks and checks sections (including `[sn76489_2]`, the second SN76489 --
see below), key aliases, name normalisation and bad values (including the
non-chip `[player]` section's `shuffle`/`skip_button`/`preview`/
`preview_seconds`/`recursive`/`root_dir` keys):

```sh
gcc -O0 -g -Wall -I shim -I ../../src/master/src \
    test_player_config.c ../../src/master/src/player_config.c \
    -o /tmp/player_config_test
/tmp/player_config_test   # prints "ok" and exits 0, or FAIL lines and exits 1
```

## SN76489 dual-chip support (VGM 0x30, header clock bit 30)

Builds its own recording slave_bus and synthetic VGMs, then asserts:
`vgm_player_scan_chips()`'s header mask includes the second SN76489
(`VGM_CHIP_SN76489_2`) exactly when the SN76489 clock field (header 0x0C)
has bit 30 set, command `0x30 dd` dispatches a `WRITE0` to
`VGM_CHIP_SN76489_2` (same wire format as `0x50 dd` for the first chip),
and both chips are `RESET` with the same clock preset (the header has only
one clock field for both). Also regression-tests `hdr_clock()`'s bit 30/31
masking -- before it was fixed, a dual-chip clock value overflowed the
sanity ceiling and made the header mask miss even the FIRST SN76489.
Self-contained -- takes no argument.

```sh
gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../src/protocol \
    test_vgm_sn76489_dual.c ../../src/master/src/vgm_player.c ../../src/master/src/vgm_chips.c \
    -o /tmp/sn76489_dual_test
/tmp/sn76489_dual_test   # prints "ok" and exits 0, or FAIL lines and exits 1
```

## Cover-art image decoding (JPEG/PNG, `[player] display = tft` only)

Links the *actual shipped* `cover_image.c`, `third_party/tjpgd/tjpgd.c`
(baseline JPEG), `third_party/miniz_tinfl` and `inflate_scratch.c` (PNG's
DEFLATE step) against real test images under `cover_test_images/`
(committed -- see `gen_cover_test_images.py`'s own doc comment for how to
regenerate them, needs Pillow; not needed to just run this test). Stubs only
`tft_cover_clear()`/`tft_cover_blit()`/`tft_panel_geom()` from `tft_panel.h`
(captured into a plain test framebuffer instead of real SPI hardware), `player_config_display_is_tft()`
(forced true), and `spi0_bus_lock()`/`spi0_bus_unlock()` (no-ops -- a single-
threaded host has no core1 TFT redraw to serialize against). This test is
what caught cover_image.c's PNG decoder treating
IDAT data as raw DEFLATE instead of zlib-wrapped DEFLATE (RFC 1950 -- a 2-byte
header + Adler32 trailer around the raw stream) -- every real PNG failed to
decode at all until `TINFL_FLAG_PARSE_ZLIB_HEADER` was added, caught
immediately by this test against real fixtures before ever reaching hardware.

Checks: a solid-color PNG wider than the cover area scales down and centers
correctly (and the letterboxed margin stays background colour); a small RGBA
PNG is centered without upscaling (alpha channel ignored, not blended); an
8-bit grayscale PNG decodes correctly; a solid-color baseline JPEG decodes to
a close (not exact -- lossy) match; a palette (color type 3) PNG's actual
PLTE lookup resolves both palette entries it uses to the right colour at
both 8 bits/pixel and, separately, 4 bits/pixel -- a small palette is very
often packed below 8 bits/pixel in real files (added 2026-10-04/05 --
real-world retro game cover art is very often palette PNGs, so this needed
supporting, not just failing gracefully); a missing file and a NULL path
both leave the area blank. Must be run from `tools/host_tests/` (relative
paths into `cover_test_images/`).

```sh
gcc -O0 -g -Wall -I shim -I ../../src/master/src -I ../../third_party/tjpgd \
    -I ../../third_party/miniz_tinfl -lm \
    test_cover_image.c ../../src/master/src/cover_image.c \
    ../../src/master/src/inflate_scratch.c \
    ../../third_party/tjpgd/tjpgd.c ../../third_party/miniz_tinfl/miniz_tinfl.c \
    -o /tmp/cover_image_test
(cd tools/host_tests && /tmp/cover_image_test)   # prints "ok" and exits 0, or FAIL lines and exits 1
```
