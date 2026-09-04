#include <stdio.h>
#include "vgm_player.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "synthetic.vgm";
    vgm_player_opts_t opts = {.loop_enabled = false, .poll_skip = NULL};
    bool ok = vgm_player_play(path, &opts);
    printf("vgm_player_play returned %s\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}
