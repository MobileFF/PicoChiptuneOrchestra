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
// ym2151, ym2203, scc (a.k.a. k051649), segapcm. Dashes/underscores/spaces
// and case are ignored. Lines starting with # or ; are comments. Unknown
// sections/keys and a missing file are warnings, never fatal -- anything
// not set keeps its built-in default (see slave_bus.c).
//
// One more section, not a chip -- general playback behaviour:
//
//   [player]
//   shuffle = no         ; yes/no (default no). When yes, main.c plays each
//                        ; pass's playlist in random order (re-shuffled every
//                        ; time the whole SD card root is replayed) instead
//                        ; of case-insensitive alphabetical order.
#pragma once

#include <stddef.h>
#include <stdbool.h>

// Parse `text` (NUL-terminated) and apply each setting via the
// slave_bus_set_* functions. Returns the number of settings applied.
// Exposed for host testing; firmware calls player_config_load().
int player_config_apply(const char *text);

// [player] shuffle = yes|no, default false. Read by main.c's playlist loop.
bool player_config_shuffle_enabled(void);

// Read `path` from the mounted filesystem and hand it to
// player_config_apply(). Call after f_mount, before slave_bus_init().
// Missing/empty file -> returns 0, defaults untouched.
int player_config_load(const char *path);

// Pick the config file off the SD card root and load it: "vgmplay.ini" if
// present, else the alphabetically-first "vgmplay*.ini" (case-insensitive)
// -- so a card can be labelled "vgmplay_scc.ini" etc. Returns the number of
// settings applied (0 = no such file). This is what main() calls.
int player_config_autoload(void);
