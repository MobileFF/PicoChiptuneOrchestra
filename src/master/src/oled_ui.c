#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "ssd1306.h"
#include "st7735.h"
#include "tft_big.h"
#include "tft_panel.h"
#include "tft_pins.h"
#include "vgm_chips.h"
#include "vgm_player.h"
#include "player_config.h"

// core1 must wait via busy_wait_*, NOT sleep_ms/sleep_us: pico-sdk's
// sleep_*() (even from core1) arms an alarm on the DEFAULT alarm pool, which
// lives on core0, then __wfe()s until core0's timer IRQ services it. If core0
// is busy for a long stretch and slow to take that IRQ (SD mount, a .vgz
// inflate + SD writes, a big YM2612 PCM-bank SPI upload), core1's __wfe never
// wakes and the render loop hangs -- panel frozen on its last frame while
// audio keeps playing. busy_wait_* just polls the hardware timer, so it is
// immune. core1 is a dedicated core; burning 150 ms busy-waiting is fine.
static inline void oled_wait_ms(uint32_t ms) { busy_wait_us((uint64_t)ms * 1000u); }
static inline void oled_wait_us(uint32_t us) { busy_wait_us(us); }

// --- pins / bus ----------------------------------------------------------
// GPIO0/1 were the UART debug log; master logging now goes over USB CDC
// only (master/CMakeLists.txt: pico_enable_stdio_uart(master 0)), freeing
// this pair for I2C0. See docs/circuit.md section 1 and design-notes.md 8.1.
#define OLED_I2C      i2c0
#define OLED_SDA_PIN  0
#define OLED_SCL_PIN  1
#define OLED_I2C_HZ   400000
#define OLED_ADDR     0x3C

// [player] display = tft: an ST7735 on the shared SPI0 bus (see st7735.h /
// spi0_bus_lock.h and docs/circuit.md). GPIOs are in tft_pins.h (shared with
// slave_bus.c's CS-collision warning). Not configurable via vgmplay.ini
// (unlike skip_button's GPIO) -- add that if a wiring actually needs it;
// these three spare, genuine-Pico-header-exposed GPIOs were free.


// --- backend selection (chosen once in oled_ui_init(), from [player] display) --

// Where each piece of the now-playing / status screen goes, in text rows
// (pages) counted from the top of the text area. Per panel: the 8-row OLED /
// ST7735 layout is the original; the big panels have more rows and columns.
#define LAYOUT_MAX_COLS 26
typedef struct {
    uint8_t cols;        // characters per text row
    uint8_t name_lines;  // now-playing filename, from page 0
    uint8_t time_page;
    uint8_t label_page;  // "Chips:"
    uint8_t chips_page;
    uint8_t chips_lines;
    uint8_t status_page; // status message in MODE_STATUS (after the 2-row title)
    uint8_t status_lines;
} display_layout_t;

typedef struct {
    bool (*init)(void);   // bring up the panel; see each backend's own init doc comment
    void (*recover)(void); // may be NULL: bus-recovery to retry after init/show fails
    void (*clear)(void);
    void (*text)(uint8_t x, uint8_t page, const char *s);
    bool (*show)(void);
    uint32_t redraw_ms;   // core1_main()'s per-frame wait -- see below
    uint8_t text_page_offset; // added to every page passed to .text() -- 0 for
                              // the OLED (uses its whole screen); for the TFT,
                              // shifts the same 8-page text layout down past
                              // st7735.h's COVER_AREA_H so cover_image.c's
                              // once-per-folder image has the rows above it
                              // (see text_at() below).
    const display_layout_t *layout;
} display_backend_t;

static void oled_i2c_bring_up(void); // defined below, near the ssd1306 init wrapper

// Set once, from oled_ui_init()'s own argument, before core1 (which actually
// calls backend_st7735_init(), via s_backend->init()) is launched -- see
// st7735_init()'s doc comment on why this must be accurate (SPI0 must NOT be
// re-initialized here when the SD card driver already brought it up first).
static bool s_sd_spi0_ready = false;

static bool backend_ssd1306_init(void) { return ssd1306_init(OLED_I2C, OLED_ADDR); }
static bool backend_st7735_init(void) {
    return st7735_init(spi0, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO, s_sd_spi0_ready);
}

static const display_layout_t LAYOUT_8ROW = {
    .cols = 21, .name_lines = 2, .time_page = 3, .label_page = 5,
    .chips_page = 6, .chips_lines = 2, .status_page = 3, .status_lines = 2,
};

static const display_backend_t BACKEND_SSD1306 = {
    .init = backend_ssd1306_init, .recover = oled_i2c_bring_up,
    .clear = ssd1306_clear, .text = ssd1306_text, .show = ssd1306_show,
    .redraw_ms = 150, // I2C0 is never shared with anything -- no reason to go slower
    .text_page_offset = 0,
    .layout = &LAYOUT_8ROW,
};
// Deliberately much slower than the OLED's 150ms (2026-10-02 finding):
// frequent SPI0 traffic from the TFT was causing the SD card to see
// FR_DISK_ERR (suspected cause: the SD card not fully releasing the shared
// MISO line / ignoring unrelated SCK activity while its own CS is
// deasserted -- see sd_spi.c's own sd_spi_deselect() comment, and
// docs/design-notes.md's TFT writeup). Confirmed fixed at 2000ms; now
// trying 500ms (still a 4x reduction from 150ms) since the status screen's
// elapsed-time counter only needs ~1s granularity to look right, so
// anything under ~1000ms already serves that without redrawing needlessly
// often. Drop lower only with real evidence it's still safe (watch for
// FR_DISK_ERR in the log); a MISO pull-up or a fully independent PIO-based
// SPI bus for the TFT are the options if faster-than-this is ever needed.
static const display_backend_t BACKEND_ST7735 = {
    .init = backend_st7735_init, .recover = NULL,
    .clear = st7735_clear, .text = st7735_text, .show = st7735_show,
    .redraw_ms = 500,
    .text_page_offset = COVER_AREA_H / 8, // 12 -- see cover_image.h
    .layout = &LAYOUT_8ROW,
};

_Static_assert(COVER_AREA_H % 8 == 0,
               "text_page_offset assumes COVER_AREA_H is a whole number of 8px pages");

// Big panels: text rows are 16px and characters 12px (tft_big.h), so cols is
// width/12 and rows is (height - cover_h)/16 -- 10 for the ILI9341 (320-160),
// 15 for the ST7796 (480-240). Cover heights are tft_panel.c's.
static const display_layout_t LAYOUT_ILI9341 = {
    .cols = 20, .name_lines = 3, .time_page = 4, .label_page = 6,
    .chips_page = 7, .chips_lines = 3, .status_page = 3, .status_lines = 3,
};
static const display_layout_t LAYOUT_ST7796 = {
    .cols = 26, .name_lines = 4, .time_page = 5, .label_page = 7,
    .chips_page = 8, .chips_lines = 4, .status_page = 3, .status_lines = 4,
};

static bool backend_tft_big_init(void) {
    return tft_big_init(spi0, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO, s_sd_spi0_ready);
}

// Same redraw interval as the ST7735: each redraw here now pushes the whole
// text area (see tft_big.h), so it isn't made any more frequent than that.
static const display_backend_t BACKEND_ILI9341 = {
    .init = backend_tft_big_init, .recover = NULL,
    .clear = tft_big_clear, .text = tft_big_text, .show = tft_big_show,
    .redraw_ms = 500,
    .text_page_offset = 0, // tft_big.c positions the text area under the cover itself
    .layout = &LAYOUT_ILI9341,
};
static const display_backend_t BACKEND_ST7796 = {
    .init = backend_tft_big_init, .recover = NULL,
    .clear = tft_big_clear, .text = tft_big_text, .show = tft_big_show,
    .redraw_ms = 500,
    .text_page_offset = 0,
    .layout = &LAYOUT_ST7796,
};

static const display_backend_t *s_backend = &BACKEND_SSD1306; // set in oled_ui_init()

// --- shared state (core0 writes via the setters, core1 reads once/frame) --

static critical_section_t s_cs;
static bool s_enabled;               // false until init succeeds; gates every setter

typedef enum { MODE_STATUS, MODE_PLAYING } ui_mode_t;

static ui_mode_t s_mode = MODE_STATUS;
static char s_text[64];               // filename, or status message
static uint32_t s_chip_mask;
static bool s_dirty = true;           // s_mode/s_text/s_chip_mask changed since last render

// core1 must NEVER printf: stdio here goes to USB CDC (TinyUSB), which is not
// multicore-safe, and core0's stdio-usb timer task pumps tud_task()
// concurrently -- a printf from core1 while core0 is busy (SD mount, a big
// YM2612 PCM-bank SPI upload, ...) and not draining USB has hung core1 dead,
// leaving the panel frozen on its last frame ("starting...") while audio
// keeps playing. So the render loop records state in these instead and core0
// prints them from vgm/main if it wants.
static volatile bool     s_oled_answered;   // panel ACKed at least once
static volatile uint32_t s_oled_reinits;    // times the render loop had to re-init a wedged panel
static volatile uint32_t s_oled_frames;     // core1 loop iterations (heartbeat -- stuck if this stops)
static volatile uint32_t s_oled_shows_ok;   // successful ssd1306_show() pushes
static volatile uint32_t s_oled_shows_fail; // failed pushes (I2C write didn't complete)
// Set once core1 has called multicore_lockout_victim_init() (always the very
// first thing core1_main() does, panel or no panel). [player] flash_cache's
// flash_disk_init() must not call multicore_lockout_start_blocking() before
// this is true -- see oled_ui_wait_for_core1_lockout_ready() below.
static volatile bool s_core1_lockout_ready;

bool     oled_ui_answered(void)      { return s_oled_answered; }
uint32_t oled_ui_reinit_count(void)  { return s_oled_reinits; }
void oled_ui_diag(uint32_t *frames, uint32_t *ok, uint32_t *fail, uint32_t *reinits) {
    if (frames)  *frames  = s_oled_frames;
    if (ok)      *ok      = s_oled_shows_ok;
    if (fail)    *fail    = s_oled_shows_fail;
    if (reinits) *reinits = s_oled_reinits;
}

bool oled_ui_wait_for_core1_lockout_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!s_core1_lockout_ready) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}

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

static const char *CHIP_LABEL[VGM_CHIP_COUNT] = {
    [VGM_CHIP_SN76489] = "SN76489",
    [VGM_CHIP_YM2413]  = "YM2413",
    [VGM_CHIP_YM2612]  = "YM2612",
    [VGM_CHIP_AY8910]  = "AY-3-8910",
    [VGM_CHIP_YM2151]  = "YM2151",
    [VGM_CHIP_YM2203]  = "YM2203",
    [VGM_CHIP_SCC]      = "SCC",
    [VGM_CHIP_SEGAPCM] = "SegaPCM",
    [VGM_CHIP_SN76489_2] = "SN76489#2",
};

// Applies the current backend's text_page_offset -- every text draw in this
// file goes through this instead of calling s_backend->text() directly, so
// the 8-page layout below is written relative to "page 0 = top of the text
// area" regardless of which backend (and therefore which offset) is active.
static void text_at(uint8_t x, uint8_t page, const char *s) {
    s_backend->text(x, page + s_backend->text_page_offset, s);
}

// Wraps `s` over `lines` text rows of layout cols characters each. If it
// doesn't fit, the last row ends in "..".
static void draw_wrapped(const char *s, uint8_t page0, uint8_t lines) {
    const size_t w = s_backend->layout->cols;
    const size_t n = strlen(s);
    const bool overflow = n > lines * w;
    char buf[LAYOUT_MAX_COLS + 1];
    for (uint8_t i = 0; i < lines; i++) {
        size_t start = i * w;
        if (start >= n) {
            buf[0] = '\0';
        } else if (overflow && i == lines - 1) {
            snprintf(buf, sizeof(buf), "%.*s..", (int)(w - 2), s + start);
        } else {
            snprintf(buf, sizeof(buf), "%.*s", (int)w, s + start);
        }
        text_at(0, page0 + i, buf);
    }
}

static void draw_chips(uint32_t mask, uint8_t page0, uint8_t lines) {
    const size_t w = s_backend->layout->cols;
    char line[4][LAYOUT_MAX_COLS + 1] = {{0}};
    int row = 0;
    size_t len = 0;

    if (mask == 0) {
        text_at(0, page0, "detecting...");
        return;
    }
    for (int c = 0; c < VGM_CHIP_COUNT; c++) {
        if (!(mask & (1u << c)) || !CHIP_LABEL[c]) continue;
        const char *name = CHIP_LABEL[c];
        size_t add = strlen(name) + (len ? 1 : 0);
        if (len + add > w) {
            if (row == lines - 1) { // no room left -- mark overflow and stop
                if (len + 1 <= w) strcat(line[row], "+");
                break;
            }
            row++;
            len = 0;
            add = strlen(name);
        }
        if (len) { strcat(line[row], " "); len++; }
        strcat(line[row], name);
        len += strlen(name);
    }
    for (uint8_t r = 0; r < lines; r++) text_at(0, page0 + r, line[r]);
}

// Returns false if the framebuffer push to the panel failed.
static bool render(ui_mode_t mode, const char *text, uint32_t chip_mask, uint32_t elapsed_s) {
    const display_layout_t *L = s_backend->layout;
    s_backend->clear();

    if (mode == MODE_STATUS) {
        // "PicoChiptuneOrchestra" is 22 chars -- one over SSD1306_COLS_PER_LINE
        // (21), so it's split at the word boundary across two pages instead
        // of relying on draw_wrapped()'s blind char-count cut (which would
        // strand a lone "a" on the second line).
        text_at(0, 0, "PicoChiptune");
        text_at(0, 1, "Orchestra");
        draw_wrapped(text, L->status_page, L->status_lines);
        return s_backend->show();
    }

    draw_wrapped(text, 0, L->name_lines);

    if (elapsed_s > 99 * 60 + 59) elapsed_s = 99 * 60 + 59;
    char t[16];
    snprintf(t, sizeof(t), "Time  %02u:%02u",
             (unsigned)(elapsed_s / 60), (unsigned)(elapsed_s % 60));
    text_at(0, L->time_page, t);

    text_at(0, L->label_page, "Chips:");
    draw_chips(chip_mask, L->chips_page, L->chips_lines);
    return s_backend->show();
}

static void core1_main(void) {
    // Registers this core as a lockout "victim": [player] flash_cache's
    // flash writes (flash_disk.c) must park core1 for the duration of every
    // flash_range_erase()/flash_range_program() call, since those stall XIP
    // for BOTH cores and core1 is normally fetching this very loop's code
    // straight out of flash. See multicore_lockout_start_blocking() there.
    multicore_lockout_victim_init();
    s_core1_lockout_ready = true;

    ui_mode_t last_mode = (ui_mode_t)-1;
    char last_text[64] = {0};
    uint32_t last_mask = 0xFFFFFFFFu;
    uint32_t last_elapsed = 0xFFFFFFFFu;

    bool panel_up = false;
    int show_fails = 0;

    for (;;) {
        s_oled_frames++; // heartbeat: if this stops incrementing, core1 is stuck
        if (!panel_up) {
            if (s_backend->init()) {
                panel_up = true;
                show_fails = 0;
                last_mode = (ui_mode_t)-1; // force a full redraw of current state
                last_text[0] = '\1';
                last_mask = last_elapsed = 0xFFFFFFFFu;
                s_oled_answered = true; // NOT printf -- see note by the decl
            } else {
                if (s_backend->recover) s_backend->recover(); // clear a possible wedge (oled only)
                oled_wait_ms(1000);
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
                s_oled_shows_ok++;
                show_fails = 0;
                last_mode = mode;
                memcpy(last_text, text, sizeof(last_text));
                last_mask = mask;
                last_elapsed = elapsed;
            } else if (s_oled_shows_fail++, ++show_fails >= 5) {
                // Panel stopped answering mid-session (unplugged, glitch,
                // wedged bus) -- drop back to the recovery path. NOT printf
                // (see note by the decl): a printf from core1 here is exactly
                // what used to hang this loop, so the panel never recovered.
                s_oled_reinits++;
                panel_up = false;
                continue;
            }
        }
        oled_wait_ms(s_backend->redraw_ms);
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
    oled_wait_us(10);
    for (int i = 0; i < 16 && !gpio_get(OLED_SDA_PIN); i++) {
        gpio_put(OLED_SCL_PIN, 0); oled_wait_us(6);
        gpio_put(OLED_SCL_PIN, 1); oled_wait_us(6);
    }
    // STOP condition: SDA low -> high while SCL is high.
    gpio_set_dir(OLED_SDA_PIN, GPIO_OUT); gpio_put(OLED_SDA_PIN, 0); oled_wait_us(6);
    gpio_put(OLED_SCL_PIN, 1); oled_wait_us(6);
    gpio_put(OLED_SDA_PIN, 1); oled_wait_us(6);
    gpio_set_dir(OLED_SDA_PIN, GPIO_IN);

    i2c_init(OLED_I2C, OLED_I2C_HZ);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);
}

void oled_ui_init(bool sd_spi0_ready) {
    critical_section_init(&s_cs);
    s_enabled = true; // record song/status from now on even if the panel is
                      // slow to appear -- core1 draws it once it's up.
    s_sd_spi0_ready = sd_spi0_ready; // read by backend_st7735_init() on core1, below

    if (player_config_display_is_tft()) {
        const char *panel = "ST7735";
        switch (player_config_tft_panel()) {
        case PLAYER_TFT_ILI9341: s_backend = &BACKEND_ILI9341; panel = "ILI9341"; break;
        case PLAYER_TFT_ST7796:  s_backend = &BACKEND_ST7796;  panel = "ST7796";  break;
        default:                 s_backend = &BACKEND_ST7735;  break;
        }
        printf("display: [player] display = tft -- %s on SPI0 "
               "(CS=GPIO%u DC=GPIO%u RST=GPIO%u), core1 render loop started\n",
               panel, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);
    } else {
        s_backend = &BACKEND_SSD1306;
        oled_i2c_bring_up();
        // Defensive (2026-10-04, FR_DISK_ERR investigation): display = oled
        // never otherwise touches TFT_CS_GPIO at all -- st7735_init() simply
        // never runs -- so if an ST7735 module happens to still be
        // physically wired to the shared SPI0 bus (e.g. while diagnosing
        // whether a problem is TFT-related by switching back to oled in
        // vgmplay.ini without physically unplugging the panel), its CS pin
        // is left floating. A floating CS on a chip that's still listening
        // to the shared SCK/MOSI could, depending on what it floats to,
        // make it think it's selected and start clocking in every SD
        // transaction alongside the SD card's own CS -- harmless to SD
        // reads on its own (TFT's MISO is unconnected, so it can't drive
        // data back), but it is one more unknown on a bus already known to
        // be sensitive (see docs/circuit.md 1.1b), so pin it high
        // (deselected) here rather than leave it floating whenever a TFT
        // isn't the active backend.
        gpio_init(TFT_CS_GPIO);
        gpio_set_dir(TFT_CS_GPIO, GPIO_OUT);
        gpio_put(TFT_CS_GPIO, 1);
        printf("display: OLED (SSD1306) -- I2C0 up (GPIO0 SDA / GPIO1 SCL), "
               "core1 render loop started\n");
    }
    oled_ui_set_status("starting...");
    // core1 owns the panel: it retries the backend's init() until the
    // display answers (immediately, for tft -- see st7735_init()'s doc
    // comment), and re-runs bus recovery if it stops answering mid-session,
    // so a slow / briefly-wedged / late-plugged oled still comes up.
    multicore_launch_core1(core1_main);
}
