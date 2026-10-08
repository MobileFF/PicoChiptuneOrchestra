// tft_pins.h -- GPIOs [player] display = tft reserves for the ST7735 status
// display (see st7735.h / oled_ui.c). Split into their own header, shared by
// oled_ui.c (which owns and drives them) and slave_bus.c (which only needs
// their numbers for its CS-collision warning), so the two definitions can't
// drift apart.
#pragma once

#define TFT_CS_GPIO  3
#define TFT_DC_GPIO  4
#define TFT_RST_GPIO 5

// SPI0's SCK/MOSI (the SD card's own wiring, shared with the TFT -- see
// spi0_bus_lock.h). Used by every TFT driver that brings SPI0 up itself.
#define TFT_SPI0_SCK_GPIO  18
#define TFT_SPI0_MOSI_GPIO 19
