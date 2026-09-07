// oled_ui.h -- status display task for the master. Owns I2C0 (GPIO0 SDA /
// GPIO1 SCL) and a second core: core0 just publishes "what's playing" with
// the setters below, core1 renders the SSD1306 on its own schedule so the
// ~23ms framebuffer push never lands inside vgm_player.c's wait_samples()
// timing. See docs/circuit.md section 1 for wiring.
//
// If no panel ACKs at init, the whole thing silently disables itself and
// every setter becomes a no-op -- the player runs identically without it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bring up I2C0 on GPIO0/1, probe+init the SSD1306, and (on success)
// launch core1's render loop. Call once, after stdio and slave_bus init.
void oled_ui_init(void);

// Diagnostics the core1 render loop records instead of printf-ing (it must
// not -- see oled_ui.c). core0 can log these. answered = the panel ACKed at
// least once; reinit_count = times the loop had to re-init a wedged panel.
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
