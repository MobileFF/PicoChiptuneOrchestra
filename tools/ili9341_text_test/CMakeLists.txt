cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(ili9341_text_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Links the REAL driver file (not a reimplementation) -- the exact
# master/src/tft_big.c and spi0_bus_lock.c that ship in master.uf2 for
# [player] tft_panel = ili9341, so a good result here specifically confirms
# the real init/text path. player_config.c and tft_panel.c are NOT linked --
# src/main.c stubs the two functions tft_big.c calls from them, so this test
# doesn't need FatFs/SD-card code at all (see src/main.c's own comment).
add_executable(ili9341_text_test
    src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/tft_big.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/spi0_bus_lock.c
)
target_include_directories(ili9341_text_test PRIVATE
    # So src/main.c can #include "tft_big.h" / "tft_panel.h" / "player_config.h" /
    # "spi0_bus_lock.h" / "tft_pins.h". (tft_big.c's OWN includes of its
    # siblings there resolve on their own via the compiler's quote-include-
    # searches-the-including-file's-own-directory rule -- this is only
    # needed for main.c, which lives elsewhere.)
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src
)

target_link_libraries(ili9341_text_test PRIVATE
    pico_stdlib
    hardware_spi
    hardware_gpio
    pico_sync
)

# Log over USB CDC -- same cable used to flash the Pico, no extra wiring.
pico_enable_stdio_usb(ili9341_text_test 1)
pico_enable_stdio_uart(ili9341_text_test 0)

# Optional override for signal-integrity testing (2026-10-08): pass
# -DILI9341_TEST_SPI_HZ=<hz> to cmake to override tft_big.c's SPI clock below
# its usual TFT_PANEL_SPI_HZ default (see tft_big.h) -- e.g. to rule out a
# specific board's wiring (long wires, breadboard, no series resistors) not
# tolerating the normal 20 MHz. Only affects THIS test target -- the real
# firmware's own TFT_PANEL_SPI_HZ is untouched. See README.md.
if(DEFINED ILI9341_TEST_SPI_HZ)
    target_compile_definitions(ili9341_text_test PRIVATE TFT_PANEL_SPI_HZ=${ILI9341_TEST_SPI_HZ})
endif()

pico_add_extra_outputs(ili9341_text_test)
