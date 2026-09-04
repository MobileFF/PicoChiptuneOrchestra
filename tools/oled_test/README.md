# oled_test -- standalone SSD1306 OLED bring-up test

A self-contained pico-sdk project (like `tools/spi_bringup_test/`) that links
**none** of the VGM player: no SD card, no SPI slaves, no multicore, no
`vgm_player`. Just I2C0 + the OLED. Use it to decide whether a blank status
display is a **hardware** fault (wiring / power / pull-ups / wrong controller /
dead panel) or a **software** one (in `master/src/oled_ui.c` or the core1
render loop).

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1)

| OLED pin | Pico (master) |
|----------|---------------|
| VCC      | 3V3           |
| GND      | GND           |
| SDA      | GPIO0         |
| SCL      | GPIO1         |

## Build

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/oled_test -B ~/dev/build-oled-test
cmake --build ~/dev/build-oled-test -j4
# -> ~/dev/build-oled-test/oled_test.uf2
```

Flaky bus (long dupont wires, weak/no pull-ups)? Rebuild at 100 kHz:

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/oled_test -B ~/dev/build-oled-test \
    -DOLED_TEST_SLOW_I2C=1
cmake --build ~/dev/build-oled-test -j4
```

## Run

1. Flash `oled_test.uf2` onto the **master** Pico (hold BOOTSEL, copy, reboot).
2. Open the USB CDC serial port (`screen /dev/ttyACM0`, picocom, PuTTY,
   Arduino Serial Monitor -- baud rate is irrelevant for USB CDC).
3. Read the log **and** watch the panel. It loops forever:
   - manual I2C bus recovery,
   - full I2C address scan (a bare module should show exactly `0x3C`),
   - SSD1306 init at `0x3C`, then `0x3D`,
   - visual patterns: `0xA5` all-pixels-on (bypasses RAM), invert, all
     off, all on, border, checkerboards, stripes, full font/text, contrast
     sweep.

## Reading the result

| What you see | Verdict |
|---|---|
| Scan finds nothing, init fails | **Hardware** -- power, SDA/SCL swapped or not on GPIO0/1, missing pull-ups (add 4.7k to 3V3). Try a shorter cable / `-DOLED_TEST_SLOW_I2C=1`. |
| Scan finds `0x3C`/`0x3D` but "init: NO ACK" | Mostly **hardware** -- flaky contact on the command path, or it's not an SSD1306 (SH1106/SSD1309 clone needs a different init + column offset). |
| Init OK but panel stays blank -- incl. the `0xA5` step | **Hardware** -- charge pump / panel VCC. Some modules need `0x8D,0x10` (external VCC) instead of `0x8D,0x14`; or the panel is dead. |
| Init OK and every pattern displays correctly | **Hardware is fine.** The blank display under the real firmware is **software**: `master/src/oled_ui.c` (core1 launch, `panel_up` state machine), init ordering in `master/src/main.c`, or a bus conflict at bring-up. |

The same guide is repeated as a comment block at the bottom of `src/main.c`.
