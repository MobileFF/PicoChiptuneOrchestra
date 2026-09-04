// vgm_player.h -- parses an already-uncompressed .vgm file and dispatches
// register writes to slave_bus in real time. See docs/design-notes.md for
// which VGM commands are forwarded vs. silently skipped.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool loop_enabled; // jump back to the VGM loop point instead of ending
    // Total number of times the looped section is played before the song
    // ends and vgm_player_play() returns. 1 = play the loop region once (no
    // repeat), 2 = play it twice (one jump back), etc. 0 = loop forever
    // until poll_skip fires. Ignored when loop_enabled is false or the file
    // has no loop point (those always play through once and end).
    uint8_t max_loops;
    // Polled periodically during playback (roughly once per parsed
    // command); returning true aborts the current song immediately (e.g.
    // "skip" button). May be NULL.
    bool (*poll_skip)(void);
    // Called once, right after the VGM header is parsed, with a bitmask of
    // the chips this song actually uses: bit (1u << <vgm_chip_id_t>) is set
    // when that chip's clock field is populated -- non-zero AND within this
    // file's header (a field past the VGM data start is command-stream bytes,
    // not a clock). May be NULL.
    void (*on_chips)(uint32_t chip_mask);
} vgm_player_opts_t;

// path is a FatFs path, e.g. "0:/song.vgm". Returns false on I/O or header
// parse failure; a user-requested skip is not a failure (returns true).
bool vgm_player_play(const char *path, const vgm_player_opts_t *opts);

// Reads only the header of `path` and reports, in *out_mask, which chips the
// song uses (bit (1u << <vgm_chip_id_t>) set when that chip's header clock
// field is populated) -- the same set vgm_player_play() would pass to
// vgm_player_opts_t.on_chips. Lets the caller skip a file up front when it
// needs a chip that isn't wired up / is disabled in vgmplay.ini
// (slave_bus_has_chip()). Returns false on open or header-parse failure
// (then *out_mask is left untouched).
bool vgm_player_scan_chips(const char *path, uint32_t *out_mask);

// Seconds of song time elapsed in the current playback, counting straight
// through loop points (so it keeps rising on a looping song rather than
// snapping back). Reads 0 before the first song; holds its last value
// between songs until the next vgm_player_play(). Just loads one 32-bit
// word -- safe to poll from another core.
uint32_t vgm_player_elapsed_seconds(void);
