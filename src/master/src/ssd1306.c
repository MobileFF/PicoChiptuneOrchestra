#include "ssd1306.h"

#include <string.h>
#include "pico/stdlib.h"
#include "font5x7.h"

// --- state -----------------------------------------------------------------

static i2c_inst_t *s_i2c;
static uint8_t s_addr;
static uint8_t s_fb[SSD1306_W * SSD1306_PAGES]; // 1 byte = 8 vertical px in a page

// All writes go out with a leading control byte: 0x00 = a stream of
// commands, 0x40 = a stream of display data. `i2c_write_timeout_us` so a
// missing panel can't wedge core1 forever.
#define I2C_TMO_US 20000

static bool wr(uint8_t ctrl, const uint8_t *data, size_t len) {
    uint8_t buf[1 + 40];
    // small command bursts only; framebuffer data uses the dedicated path
    if (len + 1 > sizeof(buf)) return false;
    buf[0] = ctrl;
    memcpy(buf + 1, data, len);
    int n = i2c_write_timeout_us(s_i2c, s_addr, buf, len + 1, false, I2C_TMO_US);
    return n == (int)(len + 1);
}

static bool cmd(const uint8_t *c, size_t len) { return wr(0x00, c, len); }

// --- init ----------------------------------------------------------------

bool ssd1306_init(i2c_inst_t *i2c, uint8_t addr) {
    s_i2c = i2c;
    s_addr = addr;

    static const uint8_t seq[] = {
        0xAE,             // display off
        0xD5, 0x80,       // clock divide / osc freq
        0xA8, 0x3F,       // multiplex ratio = 1/64
        0xD3, 0x00,       // display offset = 0
        0x40,             // start line = 0
        0x8D, 0x14,       // charge pump on
        0x20, 0x00,       // memory addressing mode = horizontal
        0xA1,             // segment remap (col 127 -> SEG0)
        0xC8,             // COM scan direction remapped
        0xDA, 0x12,       // COM pins hardware config
        0x81, 0xCF,       // contrast
        0xD9, 0xF1,       // pre-charge period
        0xDB, 0x40,       // VCOMH deselect level
        0xA4,             // resume to RAM content
        0xA6,             // normal (non-inverted)
        0xAF,             // display on
    };
    // A just-powered SSD1306 does not ACK on I2C for the first ~100 ms, and
    // this runs very early in boot (before the SD mount). Wait, then retry a
    // few times rather than disabling the display on one transient miss.
    for (int attempt = 0; attempt < 5; attempt++) {
        // busy_wait, not sleep_ms: this runs on core1 and sleep_ms there
        // depends on core0 servicing the default alarm pool's timer IRQ,
        // which can stall for a long time during SD/SPI-heavy boot -- see
        // the note in oled_ui.c.
        busy_wait_us((attempt == 0 ? 100u : 25u) * 1000u);
        if (cmd(seq, sizeof(seq))) {
            ssd1306_clear();
            ssd1306_show();
            return true;
        }
    }
    return false;
}

// --- framebuffer -------------------------------------------------------------

void ssd1306_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

void ssd1306_text(uint8_t x, uint8_t page, const char *s) {
    if (page >= SSD1306_PAGES) return;
    uint8_t *row = &s_fb[page * SSD1306_W];
    for (; *s; s++) {
        if (x + 6 > SSD1306_W) break;
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = FONT5x7[c - 0x20];
        // page-aligned text: the glyph's 7 rows map straight onto bits 0..6
        // of this page's bytes, so it's a plain copy -- no bit shifting.
        row[x + 0] = g[0];
        row[x + 1] = g[1];
        row[x + 2] = g[2];
        row[x + 3] = g[3];
        row[x + 4] = g[4];
        row[x + 5] = 0x00; // inter-character gap
        x += 6;
    }
}

// --- push --------------------------------------------------------------------

bool ssd1306_show(void) {
    static const uint8_t win[] = {
        0x21, 0x00, SSD1306_W - 1, // column address range 0..127
        0x22, 0x00, SSD1306_PAGES - 1, // page address range 0..7
    };
    if (!cmd(win, sizeof(win))) return false;

    // One 0x40-tagged data stream for the whole framebuffer. `txbuf` is
    // static, not on the stack: this is ~1KB and ssd1306_show() runs on
    // core1, whose stack is only 2KB. ssd1306.c is single-threaded (core1
    // only, after init), so one shared scratch buffer is safe.
    static uint8_t txbuf[1 + sizeof(s_fb)];
    txbuf[0] = 0x40;
    memcpy(txbuf + 1, s_fb, sizeof(s_fb));
    int n = i2c_write_timeout_us(s_i2c, s_addr, txbuf, sizeof(txbuf), false, I2C_TMO_US * 4);
    return n == (int)sizeof(txbuf);
}
