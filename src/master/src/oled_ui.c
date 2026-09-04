#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "ssd1306.h"
#include "vgm_chips.h"
#include "vgm_player.h"

// --- pins / bus ----------------------------------------------------------
// GPIO0/1 were the UART debug log; master logging now goes over USB CDC
// only (master/CMakeLists.txt: pico_enable_stdio_uart(master 0)), freeing
// this pair for I2C0. See docs/circuit.md section 1 and design-notes.md 8.1.
#define OLED_I2C      i2c0
#define OLED_SDA_PIN  0
#define OLED_SCL_PIN  1
#define OLED_I2C_HZ   400000
#define OLED_ADDR     0x3C

// --- shared state (core0 writes via the setters, core1 reads once/frame) --

static critical_section_t s_cs;
static bool s_enabled;               // false until init succeeds; gates every setter

typedef enum { MODE_STATUS, MODE_PLAYING } ui_mode_t;

static ui_mode_t s_mode = MODE_STATUS;
static char s_text[64];               // filename, or status message
static uint32_t s_chip_mask;
static bool s_dirty = true;           // s_mode/s_text/s_chip_mask changed since last render

static void publish(ui_mode_t mode, const char *text, uint32_t chip_mask) {
    if (!s_enabled) return;
    critical_section_enter_blocking(&s_cs);
    s_mode = mode;
    snprintf(s_text, sizeof(s_text), "%s", text ? text : "");
    s_chip_mask = chip_mask;
    s_dirty = true;
    critical_section_exit(&s_cs);
}

void oled_ui_set_song(const char *fname)     { publish(MODE_PLAYING, fname, 0); }
void oled_ui_set_status(const char *msg)     { publish(MODE_STATUS, msg, 0); }

void oled_ui_set_chips(uint32_t chip_mask) {
    if (!s_enabled) return;
    critical_section_enter_blocking(&s_cs);
    s_chip_mask = chip_mask;
    s_dirty = true;
    critical_section_exit(&s_cs);
}

// --- rendering (core1 only) --------------------------------------------------

static void oled_i2c_bring_up(void); // defined below; also called from core1 on recovery

static const char *CHIP_LABEL[VGM_CHIP_COUNT] = {
    [VGM_CHIP_SN76489] = "SN76489",
    [VGM_CHIP_YM2413]  = "YM2413",
    [VGM_CHIP_YM2612]  = "YM2612",
    [VGM_CHIP_AY8910]  = "AY-3-8910",
    [VGM_CHIP_YM2151]  = "YM2151",
    [VGM_CHIP_YM2203]  = "YM2203",
    [VGM_CHIP_SCC]      = "SCC",
    [VGM_CHIP_SEGAPCM] = "SegaPCM",
};

// Split `s` across two page rows of SSD1306_COLS_PER_LINE (21) chars. The
// second row gets a trailing ".." if the string is longer than both rows.
static void draw_wrapped(const char *s, uint8_t page0) {
    char l0[SSD1306_COLS_PER_LINE + 1];
    char l1[SSD1306_COLS_PER_LINE + 1];
    size_t n = strlen(s);
    size_t w = SSD1306_COLS_PER_LINE;

    snprintf(l0, sizeof(l0), "%.*s", (int)w, s);
    if (n <= w) {
        l1[0] = '\0';
    } else if (n <= 2 * w) {
        snprintf(l1, sizeof(l1), "%s", s + w);
    } else {
        snprintf(l1, sizeof(l1), "%.*s..", (int)(w - 2), s + w);
    }
    ssd1306_text(0, page0, l0);
    ssd1306_text(0, page0 + 1, l1);
}

static void draw_chips(uint32_t mask, uint8_t page0) {
    char line[2][SSD1306_COLS_PER_LINE + 1] = {{0}, {0}};
    int row = 0;
    size_t len = 0;

    if (mask == 0) {
        ssd1306_text(0, page0, "detecting...");
        return;
    }
    for (int c = 0; c < VGM_CHIP_COUNT; c++) {
        if (!(mask & (1u << c)) || !CHIP_LABEL[c]) continue;
        const char *name = CHIP_LABEL[c];
        size_t add = strlen(name) + (len ? 1 : 0);
        if (len + add > SSD1306_COLS_PER_LINE) {
            if (row == 1) { // no room left -- mark overflow and stop
                if (len + 1 <= SSD1306_COLS_PER_LINE) strcat(line[row], "+");
                break;
            }
            row = 1;
            len = 0;
            add = strlen(name);
        }
        if (len) { strcat(line[row], " "); len++; }
        strcat(line[row], name);
        len += strlen(name);
    }
    ssd1306_text(0, page0, line[0]);
    ssd1306_text(0, page0 + 1, line[1]);
}

// Returns false if the framebuffer push to the panel failed.
static bool render(ui_mode_t mode, const char *text, uint32_t chip_mask, uint32_t elapsed_s) {
    ssd1306_clear();

    if (mode == MODE_STATUS) {
        // "PicoChiptuneOrchestra" is 22 chars -- one over SSD1306_COLS_PER_LINE
        // (21), so it's split at the word boundary across two pages instead
        // of relying on draw_wrapped()'s blind char-count cut (which would
        // strand a lone "a" on the second line).
        ssd1306_text(0, 0, "PicoChiptune");
        ssd1306_text(0, 1, "Orchestra");
        draw_wrapped(text, 3);
        return ssd1306_show();
    }

    draw_wrapped(text, 0);                 // filename, pages 0-1

    if (elapsed_s > 99 * 60 + 59) elapsed_s = 99 * 60 + 59;
    char t[16];
    snprintf(t, sizeof(t), "Time  %02u:%02u",
             (unsigned)(elapsed_s / 60), (unsigned)(elapsed_s % 60));
    ssd1306_text(0, 3, t);                 // elapsed, page 3

    ssd1306_text(0, 5, "Chips:");          // page 5
    draw_chips(chip_mask, 6);              // pages 6-7
    return ssd1306_show();
}

static void core1_main(void) {
    ui_mode_t last_mode = (ui_mode_t)-1;
    char last_text[64] = {0};
    uint32_t last_mask = 0xFFFFFFFFu;
    uint32_t last_elapsed = 0xFFFFFFFFu;

    bool panel_up = false;
    int show_fails = 0;

    for (;;) {
        if (!panel_up) {
            if (ssd1306_init(OLED_I2C, OLED_ADDR)) {
                panel_up = true;
                show_fails = 0;
                last_mode = (ui_mode_t)-1; // force a full redraw of current state
                last_text[0] = '\1';
                last_mask = last_elapsed = 0xFFFFFFFFu;
                printf("OLED: SSD1306 answered at 0x%02X\n", OLED_ADDR);
            } else {
                oled_i2c_bring_up(); // clear a possible wedge, then wait and retry
                sleep_ms(1000);
                continue;
            }
        }

        ui_mode_t mode;
        char text[64];
        uint32_t mask;
        bool dirty;

        critical_section_enter_blocking(&s_cs);
        mode = s_mode;
        memcpy(text, s_text, sizeof(text));
        mask = s_chip_mask;
        dirty = s_dirty;
        s_dirty = false;
        critical_section_exit(&s_cs);

        uint32_t elapsed = (mode == MODE_PLAYING) ? vgm_player_elapsed_seconds() : 0;

        bool changed = dirty || mode != last_mode ||
                       strcmp(text, last_text) != 0 || mask != last_mask ||
                       elapsed != last_elapsed;
        if (changed) {
            if (render(mode, text, mask, elapsed)) {
                show_fails = 0;
                last_mode = mode;
                memcpy(last_text, text, sizeof(last_text));
                last_mask = mask;
                last_elapsed = elapsed;
            } else if (++show_fails >= 5) {
                // Panel stopped answering mid-session (unplugged, glitch,
                // wedged bus) -- drop back to the recovery path.
                printf("OLED: panel stopped responding, re-initialising\n");
                panel_up = false;
                continue;
            }
        }
        sleep_ms(150);
    }
}

// --- init ------------------------------------------------------------------

// Bring up I2C0 for the panel, first clearing a wedged bus. If the master
// was reset (BOOTSEL reflash, RUN pin, brownout) partway through an I2C
// write, the SSD1306 can be left holding SDA low mid-byte -- no START can
// then be generated and every transfer fails, so the display stays dark for
// the whole session even though the panel is fine. Manually clock SCL until
// the panel releases SDA, emit a STOP, then hand the pins to the I2C block.
// Safe no-op when the bus is already idle. Called at boot and on recovery.
static void oled_i2c_bring_up(void) {
    gpio_init(OLED_SCL_PIN); gpio_set_dir(OLED_SCL_PIN, GPIO_OUT); gpio_put(OLED_SCL_PIN, 1);
    gpio_init(OLED_SDA_PIN); gpio_set_dir(OLED_SDA_PIN, GPIO_IN);  gpio_pull_up(OLED_SDA_PIN);
    sleep_us(10);
    for (int i = 0; i < 16 && !gpio_get(OLED_SDA_PIN); i++) {
        gpio_put(OLED_SCL_PIN, 0); sleep_us(6);
        gpio_put(OLED_SCL_PIN, 1); sleep_us(6);
    }
    // STOP condition: SDA low -> high while SCL is high.
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

void oled_ui_init(void) {
    critical_section_init(&s_cs);
    s_enabled = true; // record song/status from now on even if the panel is
                      // slow to appear -- core1 draws it once it's up.
    oled_i2c_bring_up();
    oled_ui_set_status("starting...");
    // core1 owns the panel: it retries ssd1306_init() until the display
    // answers, and re-runs bus recovery if it stops answering mid-session,
    // so a slow / briefly-wedged / late-plugged panel still comes up.
    multicore_launch_core1(core1_main);
    printf("OLED: I2C0 up (GPIO0 SDA / GPIO1 SCL), core1 render loop started\n");
}
