cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(sd_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI
                  ${CMAKE_BINARY_DIR}/FatFs_SPI)

# Links the REAL SD wiring/glue (not a reimplementation): master/src/
# hw_config.c is the exact pin/baud config the real firmware uses, and
# FatFs_SPI/src/glue.c (pulled in via the FatFs_SPI subdirectory above) now
# calls into flash_disk.c/spi0_bus_lock.c for this project's [player]
# flash_cache + TFT-sharing additions, so those have to be linked too for it
# to build -- they're inert for this test (pdrv "1:" is never touched, and
# nothing else contends for SPI0 here), just required to satisfy glue.c's
# references to them.
add_executable(sd_test
    src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/hw_config.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/flash_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src/spi0_bus_lock.c
)
target_include_directories(sd_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/master/src
)
target_link_libraries(sd_test PRIVATE
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_gpio
    hardware_flash
    hardware_sync
    pico_sync
    FatFs_SPI
)

# Log over USB CDC -- same cable used to flash the Pico, no extra wiring.
pico_enable_stdio_usb(sd_test 1)
pico_enable_stdio_uart(sd_test 0)

pico_add_extra_outputs(sd_test)
