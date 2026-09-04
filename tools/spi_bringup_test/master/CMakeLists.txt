cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(spi_bringup_test_master C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(spi_bringup_test_master src/main.c)
target_link_libraries(spi_bringup_test_master PRIVATE pico_stdlib hardware_spi hardware_gpio)
# -DBRINGUP_BURST=ON : hold CS across the whole 3-byte frame instead of pulsing per byte.
# -DBRINGUP_CS=<n>   : CS GPIO (default 20 = YM2151's).
if(BRINGUP_BURST)
    target_compile_definitions(spi_bringup_test_master PRIVATE BRINGUP_BURST=1)
endif()
# -DBRINGUP_MODE1=ON : SPI mode 1 (CPOL=0, CPHA=1). Must match the slave build.
if(BRINGUP_MODE1)
    target_compile_definitions(spi_bringup_test_master PRIVATE BRINGUP_MODE1=1)
endif()
if(DEFINED BRINGUP_CS)
    target_compile_definitions(spi_bringup_test_master PRIVATE BRINGUP_CS=${BRINGUP_CS})
endif()
pico_enable_stdio_uart(spi_bringup_test_master 1)
pico_enable_stdio_usb(spi_bringup_test_master 1)
pico_add_extra_outputs(spi_bringup_test_master)
