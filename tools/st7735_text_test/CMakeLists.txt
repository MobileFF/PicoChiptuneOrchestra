cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(st7735_text_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Links the REAL driver files (not a reimplementation, unlike
# tools/oled_test/'s own from-scratch SSD1306 driver) -- the exact
# master/src/st7735.c and spi0_bus_lock.c that ship in master.uf2, so a good
# result here specifically confirms the real text/font rendering path.
add_executable(st7735_text_test
    src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/st7735.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/spi0_bus_lock.c
)
target_include_directories(st7735_text_test PRIVATE
    # So src/main.c can #include "st7735.h" / "spi0_bus_lock.h" / "tft_pins.h".
    # (st7735.c's OWN includes of its siblings there resolve on their own via
    # the compiler's quote-include-searches-the-including-file's-own-directory
    # rule -- this is only needed for main.c, which lives elsewhere.)
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src
)

target_link_libraries(st7735_text_test PRIVATE
    pico_stdlib
    hardware_spi
    hardware_gpio
    pico_sync
)

# Log over USB CDC -- same cable used to flash the Pico, no extra wiring.
pico_enable_stdio_usb(st7735_text_test 1)
pico_enable_stdio_uart(st7735_text_test 0)

pico_add_extra_outputs(st7735_text_test)
