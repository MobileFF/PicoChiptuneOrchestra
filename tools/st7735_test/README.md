# st7735_test -- standalone ST7735 TFT bring-up test (MicroPython)

A single self-contained MicroPython script, no dependency on this project's
own C firmware at all: no SD card, no SPI slaves, no multicore, no
`vgm_player`. Same purpose as [`tools/oled_test/`](../oled_test/) for the
SSD1306, just MicroPython instead of a pico-sdk build -- there's nothing in
this panel's init that needs compiled C, and flashing MicroPython once then
editing/re-running one `.py` file is much faster to iterate on than
rebuilding the real `master.uf2` every time. Use it to decide whether a
blank TFT is a **hardware** fault (wiring / power / dead panel / wrong
controller) or a **software** one (in `master/src/st7735.c`, `oled_ui.c`'s
backend wiring, or boot ordering).

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1.1b)

| ST7735 pin | Pico (master) | Notes |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SCK | GPIO18 | SPI0 -- shared with the SD card in the real firmware; irrelevant here, nothing else uses SPI0 in this test |
| SDA (MOSI) | GPIO19 | SPI0 |
| CS | GPIO3 | this panel's own chip-select |
| DC (A0/RS) | GPIO4 | data/command select |
| RST | GPIO5 | reset |
| BL | 3V3 | most modules just need power to light the backlight |
| MISO (SDO) | leave unconnected | this driver, like the real one, never reads from the panel |

## Setup

1. Flash a MicroPython UF2 onto the **master** Pico (hold BOOTSEL, copy the
   `.uf2`, reboot) -- a completely separate firmware from this project's own
   `master.uf2`. Get one from
   [micropython.org/download](https://micropython.org/download/) (the
   generic "Pico" / "Pico 2" build, no extra modules needed).
2. Copy `st7735_test.py` onto the board and run it. Easiest via
   [Thonny](https://thonny.org/) (`File > Save As... > Raspberry Pi Pico`,
   then just open/run it), or from a terminal:
   ```sh
   pip install mpremote
   mpremote cp st7735_test.py :main.py
   mpremote reset
   mpremote repl   # watch the log; Ctrl-] to exit
   ```
3. Watch the panel **and** the serial/REPL log together.
4. When done, re-flash this project's own `firmware/pico1/master.uf2` to
   return the board to normal use (MicroPython replaces it entirely).

## What it does

Sends the exact same ST7735 init sequence as `master/src/st7735.c` (so a
working result here means the real driver's init is correct and the problem
is elsewhere -- the SD-card-shared wiring, boot ordering, multicore, etc.),
then fills the whole screen red, green, blue, white in a loop, 2 seconds
each, printing progress to the log so you can match what you see to what it
just sent.

## Reading the result

| What you see | Verdict |
|---|---|
| Nothing at all, ever (screen stays blank/dark) | **Hardware** -- check VCC/GND/BL first (a dim or unlit backlight often means power, not SPI), then CS/DC/RST/SCK/MOSI wiring and that they land on the GPIOs above, not the real firmware's I2C0 pins (0/1). Try a shorter cable. |
| Colors appear but look wrong (e.g. red shows as blue, or the image looks mirrored/shifted) | **Panel variant, not a fault.** Edit `MADCTL` near the top of the script (`0xC8` <-> `0xC0` swaps RGB/BGR) and re-run; for a shifted/offset image see `master/src/st7735.h`'s `ST7735_XSTART`/`ST7735_YSTART` comment for the same adjustment in the real driver. |
| Colors fill correctly | **Hardware is fine, and so is `st7735.c`'s init sequence.** A blank TFT under the real firmware is a **software/integration** issue elsewhere -- check `[player] display = tft` is actually set, `oled_ui.c`'s backend selection and `core1_main()`'s init retry loop, or a boot-ordering/SPI0-sharing problem with the SD card (`spi0_bus_lock.c`, `docs/design-notes.md`'s TFT writeup). |

If you found the right `MADCTL` (and, if needed, `XSTART`/`YSTART`) for your
specific module, carry those same values over to `master/src/st7735.h`'s
`ST7735_MADCTL`/`ST7735_XSTART`/`ST7735_YSTART` defines so the real firmware
picks them up too.
