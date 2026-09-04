// Offline-render shim for pico/stdlib.h.
//
// Same as tools/host_tests/shim/pico/stdlib.h, EXCEPT sleep_until() is a real
// extern (not a no-op inline): render_wav.c implements it so that every
// wait_samples() in vgm_player.c drives that many output samples through the
// AY / SCC emulators. get_absolute_time() stays pinned at 0, so the argument
// sleep_until() receives is "microseconds since song start".
#pragma once
#include <stdint.h>
#include <unistd.h>
#ifndef uint
typedef unsigned int uint;
#endif
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) { return t + us; }
void sleep_until(absolute_time_t t); // implemented in render_wav.c
