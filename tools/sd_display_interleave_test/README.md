# sd_display_interleave_test -- SD + display dual-core concurrency stress test

A self-contained pico-sdk project, like `tools/sd_test/` and
`tools/st7735_text_test/`, but testing ONE specific question in isolation:
**does core1 continuously, asynchronously redrawing the status display --
forever, with no synchronization to core0 beyond whatever the real drivers
already do -- while core0 continuously hammers the SD card, ever produce a
FatFs/SD error?** No config parsing, no slave bus, no `vgm_player`, no
settle window -- just mount the card, launch core1's redraw loop, and let
core0 spin `f_findfirst("0:")` forever.

**2026-10-05 revision:** the first version of this test ran everything on
one core (strictly alternating "draw a frame, then do one SD read") and
passed cleanly on both builds, thousands of iterations. That is NOT what
the real firmware does: there, core1's render loop runs forever on its own,
truly concurrently with whatever core0 happens to be doing -- the two
cores' bus transactions can land at any relative offset, including
overlapping, which a single-core alternating loop can never reproduce. This
revision launches a real core1 (`multicore_launch_core1`) that redraws
forever unpaced, while core0 hammers the SD card unpaced, so the full space
of relative dual-core timings actually gets explored -- matching an
observation from the real firmware's own investigation: the exact point an
otherwise-identical run first failed varied from run to run, which is
itself a hint that dual-core timing (not any single deterministic code
path) is what matters. See `docs/design-notes.md`'s FR_DISK_ERR
investigation for the full context (TFT/SD SPI0 sharing, `flash_cache`,
recursion, folder choice, SPI clock speed, compiler optimization, core0
stack usage, the ini parser, and SD card identity -- two different cards --
were all ruled out there).

**Third revision, same day:** dual-core SD+display concurrency alone (both
builds, several minutes, zero failures) turned out not to reproduce it
either. The one remaining thing the real firmware does that this test
hadn't covered yet is SPI1 traffic to the slave boards
(`slave_bus_init()`/`slave_bus_reset()`, toggling SCK/MOSI on GPIO10/11
plus CS on GPIO12/13/14/15/20/21/22/26 -- physically close to SPI0's own
GPIO16-19 on this board's wiring). Added: core0 now also pulses every
known chip's reset over SPI1 once per iteration, right alongside its SD
read, testing whether SPI1 activity -- a separate RP2040 peripheral, but on
nearby pins -- disturbs SPI0 through crosstalk/ground bounce.

Links the **real** drivers (`master/src/hw_config.c`, `master/src/st7735.c`
or `master/src/ssd1306.c`, `master/src/spi0_bus_lock.c`), not
reimplementations, so a result here is directly meaningful about the real
firmware's own code.

Two builds from the same `src/main.c` (switched by `TEST_USE_TFT`, see
`CMakeLists.txt`):

- **`sd_oled_interleave_test`** -- display is the SSD1306 OLED on I2C0, a
  physically separate bus from the SD card's SPI0 entirely.
- **`sd_tft_interleave_test`** -- display is the ST7735 TFT on SPI0, which
  **shares** the SD card's own physical bus (CS on its own GPIO3, same
  SCK/MOSI as the SD card).

Running the same test on both isolates "is this specifically about sharing
SPI0 with the TFT" from "does ANY concurrent display activity, on ANY bus,
ever disturb SD reads".

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1)

| Signal | Pico (master) |
|---|---|
| SD MISO | GPIO16 (SPI0) |
| SD CS | GPIO17 |
| SD SCK | GPIO18 (SPI0, shared with TFT) |
| SD MOSI | GPIO19 (SPI0, shared with TFT) |
| TFT CS | GPIO3 (`sd_tft_interleave_test` only) |
| TFT DC | GPIO4 (`sd_tft_interleave_test` only) |
| TFT RST | GPIO5 (`sd_tft_interleave_test` only) |
| OLED SDA | GPIO0, I2C0 (`sd_oled_interleave_test` only) |
| OLED SCL | GPIO1, I2C0 (`sd_oled_interleave_test` only) |

## Build

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/sd_display_interleave_test -B ~/dev/build-sd-display-interleave
cmake --build ~/dev/build-sd-display-interleave -j4
# -> ~/dev/build-sd-display-interleave/sd_oled_interleave_test.uf2
# -> ~/dev/build-sd-display-interleave/sd_tft_interleave_test.uf2
```

## Run

1. Flash one of the two `.uf2` files onto the **master** Pico (hold
   BOOTSEL, copy, reboot).
2. Open the USB CDC serial port.
3. Watch the running `iter`/`frame`/`sd_ok`/`sd_fail` counters (core0
   prints them every 500 iterations, and immediately whenever a failure
   happens). Leave it running for several minutes -- an intermittent,
   timing-dependent issue may take many thousands of iterations on BOTH
   cores before the right relative offset happens to occur.
4. When done, re-flash this project's own `firmware/pico1/master.uf2`.

## Reading the result

| What you see | Verdict |
|---|---|
| `f_mount FAILED` immediately | Hardware -- same as `tools/sd_test/`'s own table: check wiring/power/card before anything else. |
| `sd_fail` stays 0 for many thousands of iterations on **both** builds | True dual-core SD+display concurrency itself is not the problem, on either bus -- look elsewhere (the specific boot-sequence timing/state the real firmware has that this test doesn't, e.g. right after `flash_disk_init()`, `slave_bus_init()`, or the slave settle window specifically). |
| `sd_fail` only climbs on `sd_tft_interleave_test`, never on `sd_oled_interleave_test` | Confirms a real SPI0-sharing concurrency problem specific to the TFT -- the SD/TFT bus-arbitration code (`spi0_bus_lock.c`, `st7735.c`) needs another look. |
| `sd_fail` climbs on **both** builds, including `sd_oled_interleave_test` (OLED never touches SPI0 at all) | Rules out SPI0 sharing entirely -- whatever is wrong is either triggered by ANY concurrent core1 activity regardless of bus (e.g. a shared power rail dipping, or core1 launch/scheduling itself), or isn't about the display at all and this test will show it regardless of which build is running (point at the SD card/wiring itself, independent of displays, same as `tools/sd_test/`). |
