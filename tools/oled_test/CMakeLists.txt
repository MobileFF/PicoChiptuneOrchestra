cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(oled_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(oled_test src/main.c)
target_link_libraries(oled_test PRIVATE pico_stdlib hardware_i2c hardware_gpio)

# Log over BOTH USB CDC and UART. USB CDC is the same cable used to flash the
# Pico, so no extra wiring is needed to read the diagnostics. UART (GPIO0/1)
# is normally the OLED's I2C pins here, so it will be silent while the test
# runs -- that's expected.
pico_enable_stdio_usb(oled_test 1)
pico_enable_stdio_uart(oled_test 0)

pico_add_extra_outputs(oled_test)
