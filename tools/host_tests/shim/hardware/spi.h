// Minimal stand-in for pico-sdk's hardware/spi.h, covering exactly the
// single type master/src/st7735.h's declarations need to parse on a host
// build (st7735_init()'s spi_inst_t* parameter). Host tests never call
// st7735_init()/st7735_show()/st7735_text() (hardware-only -- that's still
// in the real, un-shimmed st7735.c, not linked into host tests at all);
// only st7735_cover_clear()/_blit() are exercised, stubbed directly in
// test_cover_image.c.
#pragma once

typedef struct spi_inst spi_inst_t;

#include <sys/types.h> // glibc's BSD-compat uint -- pico-sdk also typedefs uint, used by st7735.h
