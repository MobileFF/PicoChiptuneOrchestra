// oled_ui.h -- status display task for the master. Despite the name, this
// drives EITHER an SSD1306 OLED (I2C0, GPIO0 SDA / GPIO1 SCL -- the default)
// OR an ST7735 TFT (SPI0, shared with the SD card -- see st7735.h), chosen
// once at init by [player] display in vgmplay.ini (player_config.h). Owns a
// second core either way: core0 just publishes "what's playing" with the
// setters below, core1 renders the panel on its own schedule so the
// framebuffer push never lands inside vgm_player.c's wait_samples() timing.
// See docs/circuit.md section 1 for wiring.
//
// oled (the default): if no panel ACKs at init, the whole thing silently
// disables itself and every setter becomes a no-op -- the player runs
// identically without it. tft: this panel is write-only and can't be probed
// the same way (see st7735_init()'s doc comment), so it always "succeeds" --
// wiring nothing at all to those pins looks the same as a fully working
// panel, only silent instead of dark.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bring up whichever panel [player] display selects (default: I2C0/SSD1306
// on GPIO0/1; see player_config_display_is_tft()) and, once it's up, launch
// core1's render loop. Call once, after stdio, slave_bus init, and
// player_config_autoload() (the display choice has to be known already).
//
// `sd_spi0_ready`: pass whether f_mount("0:") already succeeded (tft only --
// ignored for oled, which is on a separate I2C0 peripheral). Forwarded to
// st7735_init() -- see its doc comment for why this must be accurate.
void oled_ui_init(bool sd_spi0_ready);

// Diagnostics the core1 render loop records instead of printf-ing (it must
// not -- see oled_ui.c). core0 can log these. answered = the panel ACKed at
// least once (oled backend only -- always true for tft once init runs, see
// this file's top comment); reinit_count = times the loop had to re-init a
// wedged panel (oled only -- tft's push can't fail the same way, so this
// stays 0 there even with nothing wired).
bool     oled_ui_answered(void);
uint32_t oled_ui_reinit_count(void);
// Snapshot of the core1 render loop's counters (any pointer may be NULL):
// frames = loop iterations so far (heartbeat -- frozen if it stops),
// ok/fail = successful / failed framebuffer pushes, reinits = wedge recoveries.
void     oled_ui_diag(uint32_t *frames, uint32_t *ok, uint32_t *fail, uint32_t *reinits);

// Now-playing filename (shown at the top; wrapped to two lines, then
// truncated). Also clears the chip list back to "detecting..." and is the
// point song-elapsed time is measured from (via vgm_player_elapsed_seconds).
void oled_ui_set_song(const char *fname);

// Which chips this song uses: bit (1u << <vgm_chip_id_t>). Matches the
// signature of vgm_player_opts_t.on_chips, so it can be wired straight in.
void oled_ui_set_chips(uint32_t chip_mask);

// Non-playing screen (startup banner line, "SD mount failed", "No .vgm
// files on card", ...). Wrapped across two lines.
void oled_ui_set_status(const char *msg);

// Blocks (bounded, up to timeout_ms) until core1 has registered itself as a
// multicore_lockout victim -- always the very first thing core1_main() does,
// so this only actually waits during the brief window right after
// oled_ui_init() launches core1. [player] flash_cache's flash_disk_init()
// must call this before its first flash write (multicore_lockout_start_
// blocking() hangs if the victim core never called multicore_lockout_
// victim_init()). Returns false if the timeout elapsed first (would mean
// core1 never got scheduled at all -- treat that as flash_cache being
// unavailable this boot, same as any other flash_disk_init() failure).
bool oled_ui_wait_for_core1_lockout_ready(uint32_t timeout_ms);
