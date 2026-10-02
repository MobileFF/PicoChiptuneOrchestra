# st7735_text_test -- standalone ST7735 text-rendering test (real driver)

A self-contained pico-sdk project, like `tools/oled_test/`, but unlike that
one it links the **real** driver files straight from `master/src/`
(`st7735.c` + `spi0_bus_lock.c` -- the exact code that ships in
`master.uf2`), not a from-scratch reimplementation. No SD card, no SPI
slaves, no multicore, no `vgm_player`, no `[player] display` config --
`st7735_init()` is called directly with the real firmware's pin numbers.

Use it after [`tools/st7735_test/`](../st7735_test/)'s MicroPython script has
already confirmed the panel lights up and the colors are right (that script
uses its own small reimplementation of the init sequence, independent of
this project's C code). This test instead answers a narrower question:
**does the actual shipped driver's text/font rendering path work correctly
on real hardware** -- positioning, clipping, the RGB565 byte-swap in
`st7735_show()`, the whole-framebuffer push timing, etc.

## Wiring (identical to the real firmware -- `docs/circuit.md` section 1.1b)

| ST7735 pin | Pico (master) |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK | GPIO18 (SPI0) |
| SDA (MOSI) | GPIO19 (SPI0) |
| CS | GPIO3 |
| DC (A0/RS) | GPIO4 |
| RST | GPIO5 |
| BL | 3V3 |
| MISO (SDO) | leave unconnected |

## Build

```sh
PICO_SDK_PATH=~/dev/pico-sdk cmake -S tools/st7735_text_test -B ~/dev/build-st7735-text-test
cmake --build ~/dev/build-st7735-text-test -j4
# -> ~/dev/build-st7735-text-test/st7735_text_test.uf2
```

## Run

1. Flash `st7735_text_test.uf2` onto the **master** Pico (hold BOOTSEL, copy,
   reboot).
2. Open the USB CDC serial port (baud rate is irrelevant for USB CDC).
3. Watch the panel and the log together. It redraws once a second forever,
   with a frame counter at the bottom so you can tell it's still alive even
   if the picture itself looks static.
4. When done, re-flash this project's own `firmware/pico1/master.uf2` to
   return the board to normal use.

## Reading the result

| What you see | Verdict |
|---|---|
| Blank/dark panel | Unexpected if the MicroPython color test already passed on this same wiring -- re-check CS/DC/RST specifically (this test uses the same pins, but double-check nothing came loose), and that you flashed this `.uf2` and not the MicroPython image. |
| Garbled pixels / noise instead of readable glyphs | Likely a **timing/signal-integrity** issue specific to this driver's full-framebuffer burst write (long wires, no series resistors, SPI baud too high for your wiring) -- try shortening wires; the SPI baud is fixed at `ST7735_SPI_BAUD_HZ` in `st7735.h` (deliberately matched to the SD card's, see that file's comment) so lowering it means editing that define and rebuilding both this test and the real firmware. |
| Text appears but is shifted, cropped on one edge, or offset from where you'd expect | Panel-variant RAM offset -- adjust `ST7735_XSTART`/`ST7735_YSTART` in `master/src/st7735.h` (same knob used for the earlier color-order fix) and rebuild both this test and the real firmware. |
| Every row of text is sharp, aligned, and un-clipped, and the frame counter increments every second | **The real driver's text/font path is confirmed working.** A problem under the real firmware at this point is specific to its integration: `[player] display = tft` actually being set, `oled_ui.c`'s backend-selection/`core1_main()` retry loop, or the SPI0/SD-card sharing (`spi0_bus_lock.c`, boot ordering -- see `docs/design-notes.md`'s TFT writeup). |

If this test looks good, the next step is confirming `display = tft` end to
end with the real `master.uf2`.
