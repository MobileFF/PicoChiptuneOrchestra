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
string handling; `test_player_config.c` stubs the three `slave_bus_set_*`
sinks and checks sections, key aliases, name normalisation and bad values:

```sh
gcc -O0 -g -Wall -I shim -I ../../src/master/src \
    test_player_config.c ../../src/master/src/player_config.c \
    -o /tmp/player_config_test
/tmp/player_config_test   # prints "ok" and exits 0, or FAIL lines and exits 1
```
