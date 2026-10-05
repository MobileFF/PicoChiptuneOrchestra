// spi0_bus_lock.h -- arbitrates the master's physical SPI0 bus (SCK/MOSI/
// MISO) between the SD card (FatFs_SPI/src/glue.c, called from core0) and,
// when [player] display = tft, the ST7735 status display (st7735.c, driven
// from core1's render loop -- see oled_ui.c). Both devices share the same
// wires with separate CS pins, so a transaction on one must never overlap
// a transaction on the other -- either would corrupt both.
//
// Not needed at all when display = oled (the default): the OLED is on I2C0,
// a completely separate peripheral, so nothing here is ever contended.
//
// A RECURSIVE mutex (pico_sync's recursive_mutex_t, under the mutex_t name
// for backwards compatibility -- see pico/mutex.h), not a critical_section_t
// or a plain non-recursive mutex: an SD card operation can take a long time
// (tens of ms for a big read/write), far too long for a spin-only lock, and
// mutex_enter_blocking()'s WFE/SEV wait doesn't depend on core0's timer/
// alarm pool the way sleep_ms() does (see oled_ui.c's core1 notes) -- safe
// to block on from either core. Recursive specifically because main.c's
// visit_dir()/cover_image.c's cover_image_show()/flash_disk.c's
// flash_disk_diskio_write() each hold this lock for a whole multi-step SD
// (or flash) operation, and glue.c's own diskio functions underneath them
// take it AGAIN per individual transaction -- with a plain mutex that nested
// re-entry from the SAME core deadlocks instantly (hit in the field,
// 2026-10-02: the very first boot-time directory scan hung forever, not
// just failed, once visit_dir() started wrapping its whole scan in this
// lock). A recursive mutex lets the same owning core re-enter freely (just
// bumping a count, released only once the outermost unlock runs) while still
// fully blocking the OTHER core until that whole outer section is done --
// exactly the serialization this bus needs, without the deadlock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Call once from core0, before core1 is launched (oled_ui_init() launches
// it) and before anything touches SPI0. Idempotent-unsafe -- call exactly
// once.
void spi0_bus_lock_init(void);

// Blocks (no timeout) until the bus is free (or is already held by this SAME
// core, in which case it just re-enters), then claims/re-claims it. Safe to
// call nested from one core -- see this header's top comment. Used by
// glue.c's SD-card diskio functions (one call per transaction) and by
// main.c/cover_image.c/flash_disk.c to hold the bus across a whole multi-
// transaction operation.
void spi0_bus_lock(void);
void spi0_bus_unlock(void);

// Like spi0_bus_lock(), but gives up after timeout_ms instead of blocking
// forever. Used by st7735.c's render loop (core1) so a long SD operation
// just costs that display frame, never a hang. Returns false on timeout
// (caller should skip this frame and retry next time).
bool spi0_bus_lock_timeout_ms(uint32_t timeout_ms);
