cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(sd_display_interleave_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI
                  ${CMAKE_BINARY_DIR}/FatFs_SPI)

set(MASTER_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src)

# Two executables, same test loop (src/main.c, #if TEST_USE_TFT-switched),
# one SD-card wiring/glue -- see src/main.c's own top comment for why both
# builds matter (isolates "shares SPI0 with SD" from "any display activity
# at all") and why slave_bus.c (SPI1 traffic to the slave boards) is linked
# too. Both link the REAL drivers, not reimplementations -- see
# tools/sd_test/CMakeLists.txt's own comment on why flash_disk.c/
# spi0_bus_lock.c must be linked too (glue.c references them) even though
# flash_cache's "1:" volume is never touched by this test.

add_executable(sd_oled_interleave_test
    src/main.c
    ${MASTER_SRC}/hw_config.c
    ${MASTER_SRC}/flash_disk.c
    ${MASTER_SRC}/spi0_bus_lock.c
    ${MASTER_SRC}/ssd1306.c
    ${MASTER_SRC}/slave_bus.c
)
target_compile_definitions(sd_oled_interleave_test PRIVATE TEST_USE_TFT=0)
target_include_directories(sd_oled_interleave_test PRIVATE ${MASTER_SRC} ${CMAKE_CURRENT_LIST_DIR}/../../src/protocol)
target_link_libraries(sd_oled_interleave_test PRIVATE
    pico_stdlib pico_multicore pico_sync
    hardware_spi hardware_gpio hardware_i2c hardware_flash hardware_sync
    FatFs_SPI
)
pico_enable_stdio_usb(sd_oled_interleave_test 1)
pico_enable_stdio_uart(sd_oled_interleave_test 0)
pico_add_extra_outputs(sd_oled_interleave_test)

add_executable(sd_tft_interleave_test
    src/main.c
    ${MASTER_SRC}/hw_config.c
    ${MASTER_SRC}/flash_disk.c
    ${MASTER_SRC}/spi0_bus_lock.c
    ${MASTER_SRC}/st7735.c
    ${MASTER_SRC}/slave_bus.c
)
target_compile_definitions(sd_tft_interleave_test PRIVATE TEST_USE_TFT=1)
target_include_directories(sd_tft_interleave_test PRIVATE ${MASTER_SRC} ${CMAKE_CURRENT_LIST_DIR}/../../src/protocol)
target_link_libraries(sd_tft_interleave_test PRIVATE
    pico_stdlib pico_multicore pico_sync
    hardware_spi hardware_gpio hardware_i2c hardware_flash hardware_sync
    FatFs_SPI
)
pico_enable_stdio_usb(sd_tft_interleave_test 1)
pico_enable_stdio_uart(sd_tft_interleave_test 0)
pico_add_extra_outputs(sd_tft_interleave_test)
