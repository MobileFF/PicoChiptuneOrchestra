# ili9341_text_test -- standalone ILI9341 text-rendering test (real driver)

A self-contained pico-sdk project, like `tools/st7735_text_test/`, but for
the `tft_big.c` driver [player] tft_panel = ili9341 (and st7796) use. Links
the **real** `src/master/src/tft_big.c` + `spi0_bus_lock.c` -- the exact code
that ships in `master.uf2` -- not a reimplementation. No SD card, no SPI
slaves, no multicore, no `vgm_player`, no `[player]` config parsing:
`player_config_tft_panel()`/`tft_panel_geom()` are stubbed in `src/main.c` to
always report ILI9341 (240x320), and `tft_big_init()` is called directly
with the real firmware's pin numbers.

This was built while ILI9341 hadn't yet displayed anything on real hardware
in this project (ST7735 and ST7796 already had) -- isolating `tft_big.c`'s
init/text path from the rest of the firmware (SD card, config parsing,
core1 scheduling) narrows down whether the problem is in that driver at
all, or in the wiring/panel/power. **ILI9341 is now confirmed working
(2026-10-08)**, both through this test and through the real firmware; the
root cause of the "nothing shows" / "sheared" symptoms this tool helped
diagnose along the way turned out to be a marginal/intermittent physical
connection (a pin-header-into-socket connection, re-seating it fixed both
panels) -- not a bug in `tft_big.c` or a wrong MADCTL/init sequence. Kept
around as a fast, isolated way to re-check this driver on a new ILI9341
board without needing an SD card or the full firmware.

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1.1b)

| ILI9341 pin | Pico (master) |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK | GPIO18 (SPI0) |
| SDA/MOSI | GPIO19 (SPI0) |
| CS | GPIO3 |
| DC (A0/RS) | GPIO4 |
| RST | GPIO5 |
| LED/BL | 3V3 (most ILI9341 breakouts have an onboard backlight resistor/transistor; some need this pulled low instead to turn ON -- check your module's datasheet if the backlight itself looks off/dim) |
| MISO/SDO | leave unconnected |

## Build

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/ili9341_text_test -B ~/dev/build-ili9341-text-test
cmake --build ~/dev/build-ili9341-text-test -j4
# -> ~/dev/build-ili9341-text-test/ili9341_text_test.uf2
```

### Optional: a slower SPI clock, to test signal integrity

A diagonal "shear" (each successive text row/band landing a bit further
right or in the wrong place) with otherwise correctly-shaped characters
points at the SPI data stream itself being misread -- a very common real-
world cause on a breadboard/long-wire setup is simply too fast a clock for
that wiring. `tft_big.h`'s `TFT_PANEL_SPI_HZ` default (20 MHz, matching the
SD card) is overridable for just this test target:

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/ili9341_text_test -B ~/dev/build-ili9341-text-test-slow -DILI9341_TEST_SPI_HZ=1000000
cmake --build ~/dev/build-ili9341-text-test-slow -j4
# -> ~/dev/build-ili9341-text-test-slow/ili9341_text_test.uf2 (1 MHz instead of 20 MHz)
```

The boot log prints the active `TFT_PANEL_SPI_HZ` so you can confirm which
build is running. If the shear disappears or shrinks at 1 MHz, that confirms
a wiring/signal-integrity problem (try shortening wires, adding ~33-100Ω
series resistors on SCK/MOSI, or a more direct connection than a breadboard)
rather than a geometry/driver bug -- then raise the override step by step
(e.g. 1 -> 4 -> 8 MHz) to find where it starts breaking down.

## Run

1. Flash `ili9341_text_test.uf2` onto the **master** Pico (hold BOOTSEL, copy,
   reboot).
2. Open/capture the USB CDC serial port (baud rate is irrelevant for USB
   CDC) -- e.g. `screen /dev/ttyACM0`, `picocom /dev/ttyACM0`, or
   `minicom -C log.txt -D /dev/ttyACM0` to save a copy as you go. `main()`
   waits 10s after boot before its first print specifically to leave time to
   plug in, open a terminal, and start a capture command by hand.
3. Watch the panel, the **Pico's own onboard LED**, and the log together. The
   log explains the LED pattern when it starts (1 blink at boot, 2 more right
   after `tft_big_init()` returns, then a steady once-a-second heartbeat in
   the main loop alongside the frame counter).
4. When done, re-flash this project's own `firmware/pico1/master.uf2` to
   return the board to normal use.

### Reading the colour ruler + Y labels (2026-10-08 revision)

The real firmware reserves the top `cover_h` rows (160 of this panel's 320)
for a folder's cover-art image (`cover_image.c`), and `tft_big_text()`'s page
numbering always starts BELOW that area (`tft_big.h`'s comment explains why
this differs from `st7735_text_test`'s `st7735_text(0, 0, ...)`, which draws
at the literal top-left instead).

This test now draws that reserved area as **10 distinct 16px colour bands**
(red, orange, yellow, green, cyan, blue, magenta, white, gray, pink, top to
bottom -- rows 0-15, 16-31, ... 144-159) instead of one solid fill, and each
text page below it is labelled with its OWN intended absolute row, e.g.
`PAGE0 Y=160` through `PAGE9 Y=304` (see `tft_big_show()`'s own
`y0 = cover_h + p*16` formula -- these labels are computed the identical way).

**On a board that's addressing rows correctly**: `PAGE0 Y=160` appears
immediately below the pink band (rows 144-159), and `PAGE9 Y=304` at the very
bottom (row 304-319) -- the colour ruler and the labels never overlap.

**If a label instead overwrites/overlaps a colour band** (this was reported
on real hardware once: the colour fill showed correctly for a moment, then
got overwritten by the text pages' own content soon after) -- that is NOT
a bug in `tft_big.c` (`tft_big_show()`'s window writes only ever target
`y >= cover_h`; re-audited, not a leftover-from-ST7735 setting either, see
design-notes.md's 2026-10-08 entry). It points at the PANEL itself not
honouring the row (RASET) address sent to it -- e.g. wrapping rows past some
value smaller than 320 back to 0. **Read off which colour band gets
overwritten first, by which `PAGE n Y=...` label** -- that tells you this
panel's real usable height (e.g. if `PAGE0 Y=160` overwrites the red band at
rows 0-15, the real wrap point is at row 160, meaning this panel only has
~160 usable rows, not 320, regardless of what the datasheet/listing says).
Report that back so `tft_panel_geom()`'s `{240, 320, 160}` in this file can
be corrected to match reality.

### Why the onboard LED, not the panel's own backlight

This project ties the panel's BL pin straight to 3V3 (always on, not
GPIO-controlled -- see `docs/circuit.md` 1.1b), and the panel itself has no
ACK/busy line at all: `spi_write_blocking()` finishes once the bytes are
clocked out whether or not anything is actually listening. That means a
totally dark panel and a HUNG OR BROWNOUT-RESET-LOOPING MCU look identical
from the outside -- a peer RP2040 project (ILI9341/ST7796 hardware-confirmed)
specifically flagged that a panel's backlight current draw can brown out a
marginal 5V/USB supply, which resets or freezes the MCU, not just the panel.
The onboard LED is independent proof the MCU itself kept running the whole
time, which a dark panel alone can't tell you -- **check it first**, before
reasoning about wiring at all:

| Onboard LED | Verdict |
|---|---|
| Never blinks at all, from power-on | The MCU isn't running this firmware -- not flashed correctly, still running the old `.uf2`, or browning out before `main()` even reaches the first blink (check the 5V/USB supply, especially if the panel is powered from the same rail and draws significant current). Nothing below this table matters until this blinks. |
| 1 blink, then nothing more | Hung or reset during `spi0_bus_lock_init()`/`tft_big_init()` (the RST pulse, or the init command sequence) -- this points at the driver/wiring interaction itself, not plain power. Worth trying a shorter/absent RST pulse experimentally, or checking for a short on CS/DC/RST. |
| 1 blink, then 2 more (init completed), then steady 1Hz heartbeat forever | **The MCU ran the whole init+text sequence without hanging or resetting.** A still-dark panel at this point is now confidently a panel/wiring/backlight problem, not firmware logic -- see the rows below. |

## Reading the result (once the onboard LED confirms the MCU is alive)

| What you see on the panel | Verdict |
|---|---|
| Nothing at all (no backlight, totally dark), but the onboard LED heartbeats normally | Backlight/power wiring specifically -- VCC/GND/LED pins, and whether this particular module's LED pin needs to be pulled LOW to turn on rather than tied to 3V3 (some modules invert this, or route LED through a transistor that needs a specific logic level -- check the module's own datasheet/silkscreen). Also worth measuring VCC at the module itself (not just at the Pico) with a multimeter while the LED above heartbeats, in case a marginal supply sags enough to starve the panel without quite resetting the MCU. |
| Backlight on, screen solid white/garbage/noise, no readable text | The panel is receiving SOME signal but not valid commands/data -- check CS/DC/RST specifically (swapped DC/RST is a common wiring mistake since they're adjacent pins on most breakouts), and that SCK/MOSI land on GPIO18/19 (shared with the SD card's own wiring, see `docs/circuit.md`). |
| Backlight on, screen solid black, no readable text | `tft_big_init()`'s full-screen clear-to-black ran, but `tft_big_show()`'s text pushes aren't reaching the panel or aren't landing in the visible area -- could be the window/CASET-RASET math if this exact panel's GRAM doesn't match the 240x320 this test assumes (see `tft_panel_geom()`'s stub in `src/main.c`), or a MADCTL mismatch so severe the text area maps outside the visible glass. Try toggling `ILI9341_MADCTL` in `tft_big.c` (rebuild this test) -- see that file's own comment for the bit layout and what to try. |
| Text appears but in the wrong place, rotated, or mirrored | MADCTL orientation (`ILI9341_MADCTL` in `tft_big.c`) doesn't match this panel's physical mounting -- see that file's comment (try the MY/MX/BGR combinations with MV left clear, since this project addresses the panel in portrait). |
| Colours look swapped (red/blue) but position is otherwise correct | MADCTL's BGR bit (0x08), not an inversion command -- see `tft_big.c`'s comment on why INVON/INVOFF is usually the wrong knob for this symptom. |
| Every row of text is sharp, aligned, and un-clipped, and the frame counter increments every second | **The real driver's init/text path is confirmed working for ILI9341.** A problem under the real firmware at this point is specific to its integration: `[player] display = tft` + `tft_panel = ili9341` actually being set (and the SD card's `vgmplay.ini` actually having been WRITTEN -- see design-notes.md's 2026-10-07 ST7796 note about unmounting the SD card after editing it on a PC), `oled_ui.c`'s backend-selection, or SPI0/SD-card sharing timing. |

If this test looks good, the next step is confirming `display = tft` +
`tft_panel = ili9341` end to end with the real `master.uf2`.
