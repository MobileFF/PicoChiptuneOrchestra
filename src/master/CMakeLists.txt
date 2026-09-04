add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI
                  ${CMAKE_BINARY_DIR}/FatFs_SPI)

add_executable(master
    src/main.c
    src/hw_config.c
    src/slave_bus.c
    src/vgm_chips.c
    src/vgm_player.c
    src/vgz_inflate.c
    src/ssd1306.c
    src/oled_ui.c
    src/player_config.c
)

target_include_directories(master PRIVATE
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_SOURCE_DIR}/third_party/miniz_tinfl
)

target_compile_definitions(master PRIVATE
    VGM_MASTER_VERBOSE_LOG=$<BOOL:${VGM_MASTER_VERBOSE_LOG}>
    VGM_MASTER_DEBUG_TRACE=$<BOOL:${VGM_MASTER_DEBUG_TRACE}>
)
# Optional boot hold-off (seconds) before the master mounts the SD card and
# starts sending frames -- gives every slave time to finish booting and be
# listening on SPI before the one-shot RESET frames go out. -DVGM_MASTER_BOOT_DELAY_S=N.
if(DEFINED VGM_MASTER_BOOT_DELAY_S)
    target_compile_definitions(master PRIVATE VGM_MASTER_BOOT_DELAY_S=${VGM_MASTER_BOOT_DELAY_S})
endif()

target_sources(master PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniz_tinfl/miniz_tinfl.c
)

target_link_libraries(master PRIVATE
    pico_stdlib
    pico_multicore
    pico_sync
    hardware_spi
    hardware_gpio
    hardware_i2c
    FatFs_SPI
)

# GPIO0/1 are now I2C0 for the SSD1306 status display (src/oled_ui.c), so
# the UART log is off -- master logging goes over USB CDC only. See
# docs/design-notes.md 8.1 and docs/circuit.md section 1.
pico_enable_stdio_uart(master 0)
pico_enable_stdio_usb(master 1)

pico_add_extra_outputs(master)
