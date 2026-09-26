// player_config.h -- optional INI-style config read from the SD card root
// (vgmplay.ini) at boot. Lets the user enable/disable each sound chip and
// remap its chip-select GPIO without rebuilding the firmware.
//
// Format (one [section] per chip, keys case-insensitive):
//
//   [sn76489]
//   enabled = yes        ; yes/no/1/0/on/off/true/false
//   cs      = 12         ; master GPIO for this slave's chip-select (0-28)
//   gap     = 40         ; optional: per-byte CS-pulse gap in microseconds
//   volume  = 100        ; optional: output level, percent of unity (0-255,
//                        ; default 100). Balances this chip against others
//                        ; sharing the analog mix -- see VGMSPI_OP_VOLUME in
//                        ; protocol/vgm_spi_protocol.h for why this can be
//                        ; needed even when every chip's own timing is correct.
//
// Section names: sn76489, ym2413, ym2612, ay8910 (a.k.a. ay-3-8910),
// ym2151, ym2203, scc (a.k.a. k051649), segapcm, sn76489_2 (a.k.a.
// sn76489-2 / "sn76489 2" -- a second physical SN76489 slave for VGM's
// "Dual Chip Support", command 0x30 dd; see vgm_player.c and
// docs/circuit.md). Dashes/underscores/spaces and case are ignored. Lines
// starting with # or ; are comments. Unknown sections/keys and a missing
// file are warnings, never fatal -- anything not set keeps its built-in
// default (see slave_bus.c).
//
// One more section, not a chip -- general playback behaviour:
//
//   [player]
//   shuffle = no         ; yes/no (default no). When yes, main.c plays each
//                        ; pass's playlist in random order (re-shuffled every
//                        ; time the whole SD card root is replayed) instead
//                        ; of case-insensitive alphabetical order.
//   skip_button = 2      ; optional: master GPIO for the "next song" button
//                        ; (0-28; to GND, internal pull-up). Default 2 (a
//                        ; genuine Pico's usual wiring) -- override for a
//                        ; clone board whose built-in button is wired
//                        ; elsewhere (e.g. a "USR" button on GPIO24).
//   preview = no         ; yes/no (default no). When yes, each song is cut
//                        ; short after `preview_seconds` of song time
//                        ; (counting through loop points) and playback
//                        ; advances to the next file, as if the skip button
//                        ; had been pressed -- for auditioning a whole card
//                        ; quickly.
//   preview_seconds = 30 ; optional: seconds per song when preview is on
//                        ; (default 30). Ignored when preview = no.
//   recursive = no       ; yes/no (default no). When no (default), only the
//                        ; SD card ROOT directory's .vgm/.vgz files are
//                        ; played (subdirectories are ignored, as always).
//                        ; When yes, main.c also walks every subdirectory
//                        ; (depth-first, bounded -- see MAX_RECURSE_DEPTH in
//                        ; main.c), playing each folder's .vgm/.vgz files in
//                        ; turn before moving to the next folder. shuffle/
//                        ; sort order still applies PER FOLDER, not globally
//                        ; across the whole card.
//   root_dir = <path>    ; optional: scan starts inside this SD-card-relative
//                        ; folder instead of the SD card root, e.g.
//                        ; "GAMES/Sega" (a leading/trailing slash or a "0:"
//                        ; drive prefix, if given, is stripped). Independent
//                        ; of `recursive` above: root_dir alone plays only
//                        ; that folder's own files (root_dir's subfolders are
//                        ; still ignored, same as the SD-root case); combine
//                        ; with `recursive = yes` to walk everything under
//                        ; root_dir instead of the whole card. Default ""
//                        ; (the SD card root). A missing/misspelled folder
//                        ; just plays nothing (logged), not a fatal error.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Parse `text` (NUL-terminated) and apply each setting via the
// slave_bus_set_* functions. Returns the number of settings applied.
// Exposed for host testing; firmware calls player_config_load().
int player_config_apply(const char *text);

// [player] shuffle = yes|no, default false. Read by main.c's playlist loop.
bool player_config_shuffle_enabled(void);

// [player] skip_button = <gpio>. -1 if not set (caller uses its own built-in
// default). Read by main.c before it gpio_inits the button pin.
int player_config_skip_button_gpio(void);

// [player] preview = yes|no, default false. Read by main.c's playlist loop.
bool player_config_preview_enabled(void);

// [player] preview_seconds = <n>, default 30. Only meaningful when
// player_config_preview_enabled() is true.
uint32_t player_config_preview_seconds(void);

// [player] recursive = yes|no, default false. Read by main.c's playlist loop.
bool player_config_recursive_enabled(void);

// [player] root_dir = <path>, default "" (SD card root). Returns a pointer to
// a static buffer holding the normalised, SD-card-relative folder to start
// scanning from -- no leading/trailing slash, no "0:" drive prefix; "" means
// the SD card root (the original default behaviour). Read by main.c before
// its first scan pass.
const char *player_config_root_dir(void);

// Read `path` from the mounted filesystem and hand it to
// player_config_apply(). Call after f_mount, before slave_bus_init().
// Missing/empty file -> returns 0, defaults untouched.
int player_config_load(const char *path);

// Pick the config file off the SD card root and load it: "vgmplay.ini" if
// present, else the alphabetically-first "vgmplay*.ini" (case-insensitive)
// -- so a card can be labelled "vgmplay_scc.ini" etc. Returns the number of
// settings applied (0 = no such file). This is what main() calls.
int player_config_autoload(void);
