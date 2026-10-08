add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI
                  ${CMAKE_BINARY_DIR}/FatFs_SPI)

add_executable(master
    src/main.c
    src/hw_config.c
    src/slave_bus.c
    src/vgm_chips.c
    src/vgm_player.c
    src/vgz_inflate.c
    src/inflate_scratch.c
    src/ssd1306.c
    src/st7735.c
    src/tft_big.c
    src/tft_panel.c
    src/oled_ui.c
    src/core_fault.c
    src/player_config.c
    src/flash_disk.c
    src/spi0_bus_lock.c
    src/cover_image.c
)

# main.c prints __DATE__/__TIME__ at boot specifically so a freshly-flashed
# board's own serial log can answer "is this actually the firmware I just
# built?" without guessing (see that printf's own comment -- a stale .uf2,
# e.g. from a Google Drive sync lag, has repeatedly cost real debugging time
# in this project). But __DATE__/__TIME__ only reflects when THIS translation
# unit was last compiled -- if a build only touches some OTHER file,
# main.c.o is untouched and the timestamp silently goes stale even though
# the binary is genuinely new. touch_main_c's only job is to mark main.c as
# modified (mtime bump, no content change) before every single build of
# `master`, forcing a real recompile -- and therefore a fresh timestamp --
# unconditionally, regardless of what else did or didn't change. Always
# considered out of date (no OUTPUT), so it always re-runs.
add_custom_target(touch_main_c
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)
add_dependencies(master touch_main_c)

target_include_directories(master PRIVATE
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_SOURCE_DIR}/third_party/miniz_tinfl
    ${CMAKE_SOURCE_DIR}/third_party/tjpgd
    # So FatFs_SPI/src/glue.c (compiled into this target -- see its INTERFACE
    # sources below) can #include "flash_disk.h" ([player] flash_cache's
    # second-volume dispatch) and "spi0_bus_lock.h" (SD/TFT SPI0 arbitration);
    # see glue.c's own comments.
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    ${CMAKE_SOURCE_DIR}/third_party/tjpgd/tjpgd.c
)

target_link_libraries(master PRIVATE
    pico_stdlib
    pico_multicore
    pico_sync
    pico_rand
    hardware_spi
    hardware_gpio
    hardware_i2c
    hardware_flash
    hardware_sync
    FatFs_SPI
)

# GPIO0/1 are now I2C0 for the SSD1306 status display (src/oled_ui.c), so
# the UART log is off -- master logging goes over USB CDC only. See
# docs/design-notes.md 8.1 and docs/circuit.md section 1.
pico_enable_stdio_uart(master 0)
pico_enable_stdio_usb(master 1)

pico_add_extra_outputs(master)
