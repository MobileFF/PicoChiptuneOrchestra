#include <stdio.h>
#include "vgm_player.h"

// Pass a looping .vgm (header loop offset at 0x1C non-zero) as argv[1].
//
// Case 1: max_loops = 0 (loop forever) -- the only way out is poll_skip, so
//         verify the player keeps parsing until the skip fires.
// Case 2: max_loops = 2 (bounded) with poll_skip = NULL -- verify the player
//         ends on its own after replaying the loop section, i.e. that a
//         looping song can no longer wedge the playlist forever.

static int g_calls = 0;
static bool poll_skip(void) {
    g_calls++;
    return g_calls > 20; // let several loop iterations happen, then stop
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "loop.vgm";

    vgm_player_opts_t opts1 = {.loop_enabled = true, .max_loops = 0, .poll_skip = poll_skip};
    bool ok1 = vgm_player_play(path, &opts1);
    printf("case 1 (loop forever): returned %s after %d poll_skip calls\n",
           ok1 ? "true" : "false", g_calls);
    if (!ok1 || g_calls <= 20) {
        printf("FAIL: expected the infinite loop to run until poll_skip tripped\n");
        return 1;
    }

    vgm_player_opts_t opts2 = {.loop_enabled = true, .max_loops = 2, .poll_skip = NULL};
    bool ok2 = vgm_player_play(path, &opts2);
    printf("case 2 (max_loops=2, no skip): returned %s\n", ok2 ? "true" : "false");
    if (!ok2) {
        printf("FAIL: bounded-loop playback should return true on its own\n");
        return 1;
    }

    printf("ok\n");
    return 0;
}
