add_executable(slave_scc
    src/main.c
    src/chip_scc.c
)

target_link_libraries(slave_scc PRIVATE slave_common)

target_compile_definitions(slave_scc PRIVATE
    VGM_RP2040_SYSCLK_KHZ=${VGM_RP2040_SYSCLK_KHZ}
    SCC_VOLUME_SLEW_HZ=${VGM_SCC_VOLUME_SLEW_HZ}
)

pico_enable_stdio_uart(slave_scc 1)
pico_enable_stdio_usb(slave_scc 1)

pico_add_extra_outputs(slave_scc)
