#include "spi0_bus_lock.h"

#include "pico/mutex.h"

static mutex_t s_spi0_mutex;

void spi0_bus_lock_init(void) { mutex_init(&s_spi0_mutex); }
void spi0_bus_lock(void) { mutex_enter_blocking(&s_spi0_mutex); }
void spi0_bus_unlock(void) { mutex_exit(&s_spi0_mutex); }

bool spi0_bus_lock_timeout_ms(uint32_t timeout_ms) {
    return mutex_enter_timeout_ms(&s_spi0_mutex, timeout_ms);
}
