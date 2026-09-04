add_executable(slave_segapcm
    src/main.c
    src/chip_segapcm.c
)

target_link_libraries(slave_segapcm PRIVATE slave_common)

pico_enable_stdio_uart(slave_segapcm 1)
pico_enable_stdio_usb(slave_segapcm 1)

pico_add_extra_outputs(slave_segapcm)

# RP2350/Pico 2 overclock (see slave_common/include/slave_overclock.h and root CMakeLists).
target_compile_definitions(slave_segapcm PRIVATE
    VGM_RP2350_SYSCLK_KHZ=${VGM_RP2350_SYSCLK_KHZ}
    VGM_RP2350_VREG_MV=${VGM_RP2350_VREG_MV}
)
