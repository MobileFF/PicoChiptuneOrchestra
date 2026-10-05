#include "spi0_bus_lock.h"

#include "pico/mutex.h"

// recursive_mutex_t, not mutex_t -- see spi0_bus_lock.h's top comment on why
// nested re-entry from the same core must not deadlock.
static recursive_mutex_t s_spi0_mutex;

void spi0_bus_lock_init(void) { recursive_mutex_init(&s_spi0_mutex); }
void spi0_bus_lock(void) { recursive_mutex_enter_blocking(&s_spi0_mutex); }
void spi0_bus_unlock(void) { recursive_mutex_exit(&s_spi0_mutex); }

bool spi0_bus_lock_timeout_ms(uint32_t timeout_ms) {
    return recursive_mutex_enter_timeout_ms(&s_spi0_mutex, timeout_ms);
}
