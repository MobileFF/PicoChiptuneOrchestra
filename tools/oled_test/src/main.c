// Standalone SSD1306 OLED bring-up / display test -- MASTER Pico only.
//
// Nothing from the VGM player is linked in: no SD card, no SPI slaves, no
// multicore, no vgm_player. Just I2C0 + the OLED. The point is to decide
// whether a blank panel is a HARDWARE problem (wiring / power / pull-ups /
// wrong controller / dead panel) or a SOFTWARE problem (something in
// master/src/oled_ui.c or the core1 render loop).
//
// Wiring (same as the real firmware -- docs/circuit.md section 1):
//     OLED VCC -> 3V3      OLED GND -> GND
//     OLED SDA -> GPIO0    OLED SCL -> GPIO1
//
// How to use:
//   1. Flash oled_test.uf2 onto the MASTER Pico (hold BOOTSEL, copy the
//      file, let it reboot).
//   2. Open the USB CDC serial port (`screen /dev/ttyACM0 115200`,
//      picocom, PuTTY, Arduino Serial Monitor, ...). No baud rate matters
//      for USB CDC.
//   3. Read the log AND look at the panel. The two together tell you where
//      the problem is -- see the decision guide at the bottom of this file
//      and in README.md.
//
// Build: see tools/oled_test/README.md (it's a self-contained pico-sdk
// project, exactly like tools/spi_bringup_test/).

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

// --- configuration -------------------------------------------------------

#define OLED_I2C      i2c0
#define OLED_SDA_PIN  0
#define OLED_SCL_PIN  1

// 400 kHz is what the real firmware uses. If the test is flaky, rebuild
// with -DOLED_TEST_SLOW_I2C=1 (or edit this) to drop to 100 kHz -- long
// dupont wires / no proper pull-ups often only work at the slower rate.
#ifndef OLED_TEST_SLOW_I2C
#define OLED_TEST_SLOW_I2C 0
#endif
#if OLED_TEST_SLOW_I2C
#define OLED_I2C_HZ   100000
#else
#define OLED_I2C_HZ   400000
#endif

// Bare 0.96"/1.3" modules are almost always 0x3C; a few strap to 0x3D.
// The test probes both.
#define OLED_ADDR_A   0x3C
#define OLED_ADDR_B   0x3D

#define SSD1306_W      128
#define SSD1306_H      64
#define SSD1306_PAGES  (SSD1306_H / 8)   // 8

// --- low-level I2C write ----------------------------------------------------

static uint8_t s_addr;   // resolved OLED address once init succeeds
static uint8_t s_fb[SSD1306_W * SSD1306_PAGES];

#define I2C_TMO_US 20000

// ctrl 0x00 = command stream, 0x40 = data stream.
static bool wr(uint8_t ctrl, const uint8_t *data, size_t len) {
    uint8_t buf[1 + 40];
    if (len + 1 > sizeof(buf)) return false;
    buf[0] = ctrl;
    memcpy(buf + 1, data, len);
    int n = i2c_write_timeout_us(OLED_I2C, s_addr, buf, len + 1, false, I2C_TMO_US);
    return n == (int)(len + 1);
}
static bool cmd(const uint8_t *c, size_t len) { return wr(0x00, c, len); }
static bool cmd1(uint8_t c) { return wr(0x00, &c, 1); }

// --- I2C bus recovery + init ----------------------------------------------

// If the Pico was reset partway through an I2C write, the OLED can be left
// holding SDA low, which wedges the bus (no START can be issued). Clock SCL
// by hand until it lets go, emit a STOP, then hand the pins to the I2C
// block. Harmless on an already-idle bus. (Copied from oled_ui.c.)
static void i2c_bring_up(void) {
    gpio_init(OLED_SCL_PIN); gpio_set_dir(OLED_SCL_PIN, GPIO_OUT); gpio_put(OLED_SCL_PIN, 1);
    gpio_init(OLED_SDA_PIN); gpio_set_dir(OLED_SDA_PIN, GPIO_IN);  gpio_pull_up(OLED_SDA_PIN);
    sleep_us(10);
    int clocked = 0;
    for (; clocked < 16 && !gpio_get(OLED_SDA_PIN); clocked++) {
        gpio_put(OLED_SCL_PIN, 0); sleep_us(6);
        gpio_put(OLED_SCL_PIN, 1); sleep_us(6);
    }
    if (clocked) printf("  bus recovery: clocked SCL %d times to free SDA\n", clocked);
    // STOP: SDA low -> high while SCL high.
    gpio_set_dir(OLED_SDA_PIN, GPIO_OUT); gpio_put(OLED_SDA_PIN, 0); sleep_us(6);
    gpio_put(OLED_SCL_PIN, 1); sleep_us(6);
    gpio_put(OLED_SDA_PIN, 1); sleep_us(6);
    gpio_set_dir(OLED_SDA_PIN, GPIO_IN);

    i2c_init(OLED_I2C, OLED_I2C_HZ);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);
}

// Probe every 7-bit address and report which ones ACK. A bare OLED module
// should show exactly one (0x3C, sometimes 0x3D).
static void i2c_scan(void) {
    printf("I2C scan on i2c0 (SDA=GPIO%d SCL=GPIO%d, %d Hz):\n",
           OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        uint8_t dummy = 0;
        int n = i2c_read_timeout_us(OLED_I2C, a, &dummy, 1, false, 2000);
        if (n >= 0) {
            printf("  device ACKed at 0x%02X%s\n", a,
                   (a == OLED_ADDR_A || a == OLED_ADDR_B) ? "  <- expected OLED" : "");
            found++;
        }
    }
    if (!found)
        printf("  NOTHING on the bus. => hardware: check VCC/GND, SDA<->GPIO0,\n"
               "     SCL<->GPIO1 (not swapped), and that the module has pull-ups\n"
               "     (most do; if yours doesn't, add 4.7k from SDA and SCL to 3V3).\n");
}

static const uint8_t INIT_SEQ[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock divide / osc freq
    0xA8, 0x3F,       // multiplex ratio = 1/64
    0xD3, 0x00,       // display offset = 0
    0x40,             // start line = 0
    0x8D, 0x14,       // charge pump on  (0x10 if this is an external-VCC panel)
    0x20, 0x00,       // memory addressing mode = horizontal
    0xA1,             // segment remap
    0xC8,             // COM scan direction remapped
    0xDA, 0x12,       // COM pins hardware config
    0x81, 0xCF,       // contrast
    0xD9, 0xF1,       // pre-charge period
    0xDB, 0x40,       // VCOMH deselect level
    0xA4,             // output follows RAM
    0xA6,             // normal (non-inverted)
    0xAF,             // display on
};

// Try the power-on sequence at `addr`. A freshly powered panel can ignore
// I2C for ~100 ms, so wait + retry.
static bool ssd1306_init_at(uint8_t addr) {
    s_addr = addr;
    for (int attempt = 0; attempt < 8; attempt++) {
        sleep_ms(attempt == 0 ? 120 : 25);
        if (cmd(INIT_SEQ, sizeof(INIT_SEQ)))
            return true;
    }
    return false;
}

// --- framebuffer -------------------------------------------------------------

static void fb_clear(void)      { memset(s_fb, 0x00, sizeof(s_fb)); }
static void fb_fill(void)       { memset(s_fb, 0xFF, sizeof(s_fb)); }

static void fb_checker(uint8_t sq) {   // sq = square size in px
    fb_clear();
    for (uint8_t y = 0; y < SSD1306_H; y++)
        for (uint8_t x = 0; x < SSD1306_W; x++)
            if (((x / sq) ^ (y / sq)) & 1)
                s_fb[(y / 8) * SSD1306_W + x] |= (uint8_t)(1u << (y & 7));
}

static void fb_hlines(void) {   // horizontal lines every other row
    fb_clear();
    for (uint8_t p = 0; p < SSD1306_PAGES; p++)
        memset(&s_fb[p * SSD1306_W], 0x55, SSD1306_W);
}

static void fb_vlines(void) {   // vertical lines every other column
    fb_clear();
    for (uint8_t p = 0; p < SSD1306_PAGES; p++)
        for (uint8_t x = 0; x < SSD1306_W; x += 2)
            s_fb[p * SSD1306_W + x] = 0xFF;
}

static void fb_border(void) {   // 1px frame around the full 128x64 area
    fb_clear();
    for (uint8_t x = 0; x < SSD1306_W; x++) {
        s_fb[x] |= 0x01;                                   // top
        s_fb[(SSD1306_PAGES - 1) * SSD1306_W + x] |= 0x80; // bottom
    }
    for (uint8_t y = 0; y < SSD1306_H; y++) {
        s_fb[(y / 8) * SSD1306_W + 0] |= (uint8_t)(1u << (y & 7));
        s_fb[(y / 8) * SSD1306_W + (SSD1306_W - 1)] |= (uint8_t)(1u << (y & 7));
    }
}

// Classic public-domain 5x7 GLCD font, ASCII 0x20..0x7E, column-major.
static const uint8_t FONT5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static void fb_text(uint8_t x, uint8_t page, const char *s) {
    if (page >= SSD1306_PAGES) return;
    uint8_t *row = &s_fb[page * SSD1306_W];
    for (; *s; s++) {
        if (x + 6 > SSD1306_W) break;
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = FONT5x7[c - 0x20];
        for (int i = 0; i < 5; i++) row[x + i] = g[i];
        row[x + 5] = 0x00;
        x += 6;
    }
}

// Push the whole framebuffer. Returns false if the panel stopped ACKing.
static bool fb_show(void) {
    static const uint8_t win[] = {
        0x21, 0x00, SSD1306_W - 1,
        0x22, 0x00, SSD1306_PAGES - 1,
    };
    if (!cmd(win, sizeof(win))) return false;
    static uint8_t txbuf[1 + sizeof(s_fb)];
    txbuf[0] = 0x40;
    memcpy(txbuf + 1, s_fb, sizeof(s_fb));
    int n = i2c_write_timeout_us(OLED_I2C, s_addr, txbuf, sizeof(txbuf), false, I2C_TMO_US * 4);
    return n == (int)sizeof(txbuf);
}

// --- test sequence -------------------------------------------------------

// One step: describe it on the log, render it, hold it. `entire_on` uses
// the 0xA5 command (all pixels lit regardless of RAM) so it also proves the
// panel works even if the RAM write path is broken.
static bool step(const char *desc, void (*build)(void), uint32_t hold_ms) {
    printf("  [show] %s\n", desc);
    if (build) build();
    if (!fb_show()) { printf("  !! fb_show() failed -- panel stopped responding\n"); return false; }
    sleep_ms(hold_ms);
    return true;
}

static void build_checker4(void) { fb_checker(4); }
static void build_checker1(void) { fb_checker(1); }
static void build_text_screen(void) {
    fb_clear();
    fb_text(0, 0, "SSD1306 OLED TEST");
    fb_text(0, 1, "master Pico / i2c0");
    fb_text(0, 2, "SDA=GP0 SCL=GP1");
    fb_text(0, 3, "addr 0x--");
    char a[10]; snprintf(a, sizeof(a), "0x%02X", s_addr);
    fb_text(6 * 5, 3, a);
    fb_text(0, 4, "!\"#$%&'()*+,-./0123");
    fb_text(0, 5, "ABCDEFGHIJKLMNOPQRS");
    fb_text(0, 6, "abcdefghijklmnopqrs");
    fb_text(0, 7, "If you can read this");
}

static int run_visual_tests(void) {
    // Command-only test first: 0xA5 lights every pixel without touching RAM.
    printf("  [cmd ] entire display ON (0xA5) -- whole panel should be lit\n");
    if (!cmd1(0xA5)) { printf("  !! command write failed\n"); return 1; }
    sleep_ms(2000);
    printf("  [cmd ] back to RAM content (0xA4)\n");
    cmd1(0xA4);

    printf("  [cmd ] invert (0xA7) then normal (0xA6)\n");
    cmd1(0xA7); sleep_ms(1200); cmd1(0xA6);

    if (!step("all pixels OFF (RAM cleared)",      fb_clear,          1500)) return 1;
    if (!step("all pixels ON  (RAM 0xFF)",         fb_fill,           2000)) return 1;
    if (!step("1px border frame (edge pixels)",    fb_border,         2000)) return 1;
    if (!step("checkerboard 4x4",                  build_checker4,    2000)) return 1;
    if (!step("checkerboard 1x1 (worst case)",     build_checker1,    2000)) return 1;
    if (!step("horizontal stripes",                fb_hlines,         2000)) return 1;
    if (!step("vertical stripes",                  fb_vlines,         2000)) return 1;
    if (!step("text / full font",                  build_text_screen, 3500)) return 1;

    // Contrast sweep -- brightness should visibly change.
    for (int cst = 0x00; cst <= 0xFF; cst += 0x3F) {
        printf("  [cmd ] contrast 0x%02X\n", cst);
        uint8_t c[2] = {0x81, (uint8_t)cst};
        cmd(c, 2);
        sleep_ms(700);
    }
    { uint8_t c[2] = {0x81, 0xCF}; cmd(c, 2); }
    return 0;
}

// --- main --------------------------------------------------------------------

int main(void) {
    stdio_init_all();
    sleep_ms(3000); // let a USB CDC terminal attach before the first print

    printf("\n\n=== SSD1306 OLED standalone test =========================\n");
    printf("i2c0  SDA=GPIO%d  SCL=GPIO%d  %d Hz\n", OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
    printf("expecting OLED at 0x%02X (or 0x%02X)\n", OLED_ADDR_A, OLED_ADDR_B);
    printf("=========================================================\n\n");

    uint32_t pass = 0;
    for (;;) {
        printf("---- pass %lu ----\n", (unsigned long)++pass);

        i2c_bring_up();
        i2c_scan();

        uint8_t addr = 0;
        if (ssd1306_init_at(OLED_ADDR_A))      addr = OLED_ADDR_A;
        else if (ssd1306_init_at(OLED_ADDR_B)) addr = OLED_ADDR_B;

        if (!addr) {
            printf("SSD1306 init: NO ACK at 0x%02X or 0x%02X.\n", OLED_ADDR_A, OLED_ADDR_B);
            printf("  If the scan above ALSO found nothing -> wiring/power/pull-ups.\n");
            printf("  If the scan found a device but init fails here -> the panel\n");
            printf("     answers I2C but rejected the command write (bad contact on\n");
            printf("     SCL/SDA, or it's not actually an SSD1306 -- e.g. an SH1106,\n");
            printf("     which needs a different init + a 2px column offset).\n");
            printf("  retrying in 3 s...\n\n");
            sleep_ms(3000);
            continue;
        }

        printf("SSD1306 init OK at 0x%02X. Running visual tests -- WATCH THE PANEL.\n", addr);
        int rc = run_visual_tests();
        if (rc) {
            printf("visual test aborted (panel stopped responding mid-run).\n");
            printf("  => intermittent contact / marginal power / too-fast I2C.\n");
            printf("     Try rebuilding with -DOLED_TEST_SLOW_I2C=1.\n\n");
            sleep_ms(2000);
            continue;
        }

        printf("visual tests done. If every pattern showed correctly, the OLED and\n");
        printf("its wiring are GOOD -- a blank panel under the real firmware is then\n");
        printf("a software issue (oled_ui.c / core1 render loop / init ordering).\n");
        printf("Looping the whole test again in 3 s.\n\n");
        sleep_ms(3000);
    }
}

// ---------------------------------------------------------------------------
// DECISION GUIDE
//
//  A) I2C scan finds NOTHING, init fails
//       -> HARDWARE. Power (VCC on 3V3, GND), SDA/SCL not swapped, SDA->GPIO0
//          SCL->GPIO1, module pull-ups present (add 4.7k to 3V3 if not).
//          Try a shorter cable / -DOLED_TEST_SLOW_I2C=1.
//
//  B) Scan FINDS a device (0x3C/0x3D) but "SSD1306 init: NO ACK"
//       -> Mostly hardware: flaky contact on the command path, or the panel
//          is not an SSD1306 (SH1106/SSD1309 clone). Reseat wires; if it's
//          an SH1106 the real firmware won't drive it as-is either.
//
//  C) Init OK, but the panel stays BLANK during the visual tests
//       (esp. the 0xA5 "entire ON" step, which bypasses RAM)
//       -> Hardware: charge-pump / panel VCC. Some modules need 0x8D,0x10
//          (external VCC) instead of 0x8D,0x14. Or the panel itself is dead.
//
//  D) Init OK and the patterns DISPLAY CORRECTLY
//       -> Hardware is fine. The blank panel under the real firmware is a
//          SOFTWARE problem: look at master/src/oled_ui.c (core1 launch,
//          panel_up state machine), init ordering in master/src/main.c, or
//          a bus conflict with something else the master brings up.
// ---------------------------------------------------------------------------
