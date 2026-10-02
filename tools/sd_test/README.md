# sd_test -- standalone SD card bring-up / read test (real SD stack)

A self-contained pico-sdk project, like `tools/oled_test/` and
`tools/st7735_text_test/`, but for the SD card. Links the **real** SD wiring
(`master/src/hw_config.c`) and the real `FatFs_SPI` library exactly as the
main firmware uses them -- no slaves, no multicore work, no OLED/TFT, no
`vgm_player`. Just mount the card, list what's on it, and read a `.vgm`/
`.vgz` file all the way through while timing it and summing its bytes.

Use it to answer "does SD card I/O actually work on this wiring" in
isolation -- including when `[player] display = tft` is also in play: this
test never touches a TFT or SPI0's shared-bus concerns at all, so if reads
are flaky even here, the SD card/wiring itself is the suspect, not the TFT
sharing the bus (see `docs/design-notes.md`'s TFT writeup for that design).

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1)

| SD card pin | Pico (master) |
|---|---|
| MISO | GPIO16 (SPI0) |
| CS | GPIO17 |
| SCK | GPIO18 (SPI0) |
| MOSI | GPIO19 (SPI0) |

## Build

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/sd_test -B ~/dev/build-sd-test
cmake --build ~/dev/build-sd-test -j4
# -> ~/dev/build-sd-test/sd_test.uf2
```

## Run

1. Flash `sd_test.uf2` onto the **master** Pico (hold BOOTSEL, copy, reboot).
2. Open the USB CDC serial port (baud rate is irrelevant for USB CDC). A 5s
   countdown at boot gives you time to connect before the log starts.
3. Read the log. It repeats the whole pass (mount, list, read-through,
   unmount) every few seconds forever, so you can watch for **intermittent**
   failures without resetting the board.
4. When done, re-flash this project's own `firmware/pico1/master.uf2`.

## What it does each pass

1. `f_mount("0:")` -- reports the FatFs error code and a plain-English guess
   if it fails (`FR_NOT_READY` vs `FR_NO_FILESYSTEM` mean different things).
2. Lists every entry at the SD card root, and one level into every
   subfolder it finds there (so e.g. `0:/ALLTEST/song.vgm` shows up even
   though nothing is directly at the root) -- use this to double-check a
   folder name you're passing to `[player] root_dir` actually matches what's
   on the card (case, typos, an extra space, ...).
3. Opens the first `.vgm`/`.vgz` it found and reads it to EOF in 512-byte
   chunks, reporting total bytes, elapsed time, throughput (KB/s), and a
   simple additive checksum of every byte read.

## Reading the result

| What you see | Verdict |
|---|---|
| `f_mount FAILED` every pass, `FR_NOT_READY` | **Hardware** -- card not responding at all: check VCC/GND, the 4 SPI wires above, that the card is actually seated, and try a different card. |
| `f_mount FAILED`, `FR_NO_FILESYSTEM` | The SPI link itself works (the card answered), but it isn't formatted FAT/FAT32 (exFAT and some SDXC factory formats aren't readable) -- reformat as FAT32. |
| Mount OK, but the listing is empty, wrong, or missing files/folders you know are there | Check the folder/file names printed here character-for-character against what you put in `vgmplay.ini`'s `root_dir` -- SD cards are case-sensitive here even though Windows mostly isn't. |
| Mount OK, listing looks right, but `f_read failed` partway through, or the checksum **changes between passes** on the same file | **Hardware -- a flaky link.** Likely marginal wiring (long wires, no pull-ups/pull-downs where your card needs them, bad solder joint) or marginal power under load. Try shortening wires or a powered USB port instead of a weak supply. |
| Mount OK, listing correct, read-through consistent (same byte count, same checksum, reasonable KB/s) across many passes | **SD card and its wiring are solid.** A problem under the real firmware at this point is elsewhere -- `[player] root_dir`/`recursive` settings, `vgmplay.ini` chip config, or (if `display = tft`) the TFT side specifically, now that SD itself is ruled out. |
