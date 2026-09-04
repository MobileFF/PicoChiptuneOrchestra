cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(spi_bringup_test_slave C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(spi_bringup_test_slave src/main.c)
target_link_libraries(spi_bringup_test_slave PRIVATE pico_stdlib hardware_spi hardware_gpio)
# -DBRINGUP_MODE1=ON : use SPI mode 1 (CPOL=0, CPHA=1) instead of mode 0.
# Mode-1 PL022 slaves can clock a CS-held continuous burst (mode 0 cannot);
# pair with the master's BRINGUP_BURST build to check that.
if(BRINGUP_MODE1)
    target_compile_definitions(spi_bringup_test_slave PRIVATE BRINGUP_MODE1=1)
endif()
pico_enable_stdio_uart(spi_bringup_test_slave 1)
pico_enable_stdio_usb(spi_bringup_test_slave 1)
pico_add_extra_outputs(spi_bringup_test_slave)
