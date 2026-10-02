# st7735_test.py -- standalone MicroPython bring-up test for an ST7735 TFT,
# independent of the master firmware (no SD card, no SPI slaves, no
# multicore, no vgm_player). Same idea as tools/oled_test/ for the SSD1306,
# just MicroPython instead of a pico-sdk build, since there's nothing in this
# panel's init that depends on compiled C -- flashing MicroPython once and
# editing/re-running this one file is much faster to iterate on than
# rebuilding the real firmware every time.
#
# Use it to decide whether "nothing shows on the TFT" is a HARDWARE problem
# (wiring / power / dead panel / wrong controller) or a SOFTWARE one (in
# master/src/st7735.c or how oled_ui.c drives it).
#
# Wiring (identical to the real firmware -- docs/circuit.md section 1.1b):
#   VCC  -> 3V3
#   GND  -> GND
#   SCK  -> GPIO18 (SPI0 -- shared with the SD card in the real firmware,
#                   irrelevant here since nothing else uses SPI0 in this test)
#   SDA (MOSI) -> GPIO19
#   CS   -> GPIO3
#   DC (A0/RS) -> GPIO4
#   RST  -> GPIO5
#   BL   -> 3V3 (most modules just need power to light the backlight)
#   MISO (SDO) -> leave unconnected (this driver, like the real one, never reads)
#
# Setup:
#   1. Flash a MicroPython UF2 onto the Pico (hold BOOTSEL, copy the .uf2,
#      reboot) -- a completely separate firmware from this project's own
#      master.uf2; re-flash master.uf2 afterwards to go back to normal use.
#   2. Copy this file onto the board and run it -- easiest via Thonny
#      (File > Save As > Raspberry Pi Pico), or from a terminal:
#        pip install mpremote
#        mpremote cp st7735_test.py :main.py
#        mpremote run st7735_test.py     # or just reset the board
#   3. Watch the panel AND the serial/REPL output together.
#
# What it does: the exact same init sequence as master/src/st7735.c (so a
# working result here means the real driver's init is correct and the
# problem is elsewhere -- wiring shared with the SD card, boot ordering,
# multicore, etc.), then fills the whole screen red / green / blue / white
# in a loop, printing progress so you can match what you see to the log.

from machine import Pin, SPI
import time

SCK_GPIO, MOSI_GPIO, CS_GPIO, DC_GPIO, RST_GPIO = 18, 19, 3, 4, 5
W, H = 128, 160          # portrait, matches ST7735_W/H in st7735.h
BAUD = 20_000_000        # same as the real firmware (shared SD card baud)

# If colors look swapped (red shows as blue, etc.) once something IS
# visible, toggle bit 0x08 here (0xC8 <-> 0xC0) -- see st7735.h's own
# ST7735_MADCTL comment for the same knob in the real driver.
# 0xC0 confirmed on real hardware 2026-10-01 (0xC8 showed red/blue swapped).
MADCTL = 0xC0

spi = SPI(0, baudrate=BAUD, polarity=0, phase=0,
          sck=Pin(SCK_GPIO), mosi=Pin(MOSI_GPIO))
cs = Pin(CS_GPIO, Pin.OUT, value=1)
dc = Pin(DC_GPIO, Pin.OUT, value=1)
rst = Pin(RST_GPIO, Pin.OUT, value=1)


def cmd(c, data=b""):
    cs.value(0)
    dc.value(0)
    spi.write(bytes([c]))
    if data:
        dc.value(1)
        spi.write(data)
    cs.value(1)


def reset():
    rst.value(1); time.sleep_ms(10)
    rst.value(0); time.sleep_ms(10)
    rst.value(1); time.sleep_ms(120)


def init():
    print("st7735_test: resetting panel...")
    reset()
    print("st7735_test: sending init sequence...")
    cmd(0x01); time.sleep_ms(150)   # SWRESET
    cmd(0x11); time.sleep_ms(255)   # SLPOUT
    cmd(0xB1, bytes([0x01, 0x2C, 0x2D]))   # FRMCTR1
    cmd(0xB2, bytes([0x01, 0x2C, 0x2D]))   # FRMCTR2
    cmd(0xB3, bytes([0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D]))  # FRMCTR3
    cmd(0xB4, bytes([0x07]))               # INVCTR
    cmd(0xC0, bytes([0xA2, 0x02, 0x84]))   # PWCTR1
    cmd(0xC1, bytes([0xC5]))               # PWCTR2
    cmd(0xC2, bytes([0x0A, 0x00]))         # PWCTR3
    cmd(0xC3, bytes([0x8A, 0x2A]))         # PWCTR4
    cmd(0xC4, bytes([0x8A, 0xEE]))         # PWCTR5
    cmd(0xC5, bytes([0x0E]))               # VMCTR1
    cmd(0x20)                              # INVOFF
    cmd(0x36, bytes([MADCTL]))             # MADCTL
    cmd(0x3A, bytes([0x05]))               # COLMOD: 16 bits/pixel
    cmd(0x13); time.sleep_ms(10)    # NORON
    cmd(0x29); time.sleep_ms(100)   # DISPON
    print("st7735_test: init sequence sent (this panel can't ACK, so this",
          "'succeeds' even with nothing wired -- see master/src/st7735.h's",
          "st7735_init() doc comment for the same caveat in the real driver)")


def fill(color565):
    cmd(0x2A, bytes([0, 0, (W - 1) >> 8, (W - 1) & 0xFF]))  # CASET
    cmd(0x2B, bytes([0, 0, (H - 1) >> 8, (H - 1) & 0xFF]))  # RASET
    cmd(0x2C)  # RAMWR
    hi, lo = (color565 >> 8) & 0xFF, color565 & 0xFF
    line = bytes([hi, lo]) * W
    cs.value(0)
    dc.value(1)
    for _ in range(H):
        spi.write(line)
    cs.value(1)


COLORS = (("RED", 0xF800), ("GREEN", 0x07E0), ("BLUE", 0x001F), ("WHITE", 0xFFFF))

init()
print("st7735_test: filling the screen RED/GREEN/BLUE/WHITE, 2s each, forever.")
print("st7735_test: nothing visible at all after a few cycles -> see")
print("             tools/st7735_test/README.md's hardware-vs-software table.")
while True:
    for name, color in COLORS:
        print("st7735_test: filling", name)
        fill(color)
        time.sleep(2)
