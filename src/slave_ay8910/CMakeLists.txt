add_executable(slave_ay8910
    src/main.c
    src/chip_ay8910.c
)

target_link_libraries(slave_ay8910 PRIVATE slave_common)

target_compile_definitions(slave_ay8910 PRIVATE
    VGM_RP2040_SYSCLK_KHZ=${VGM_RP2040_SYSCLK_KHZ}
    AY8910_LEVEL_SLEW_HZ=${VGM_AY8910_LEVEL_SLEW_HZ}
)

pico_enable_stdio_uart(slave_ay8910 1)
pico_enable_stdio_usb(slave_ay8910 1)

pico_add_extra_outputs(slave_ay8910)
