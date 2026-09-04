set(YMFM_DIR ${CMAKE_SOURCE_DIR}/third_party/ymfm)

add_executable(slave_ym2151
    src/main.c
    src/chip_ym2151.cpp
    ${YMFM_DIR}/src/ymfm_opm.cpp
)

target_include_directories(slave_ym2151 PRIVATE ${YMFM_DIR}/src)

target_link_libraries(slave_ym2151 PRIVATE slave_common)

pico_enable_stdio_uart(slave_ym2151 1)
pico_enable_stdio_usb(slave_ym2151 1)

pico_add_extra_outputs(slave_ym2151)

# RP2350/Pico 2 overclock (see slave_common/include/slave_overclock.h and root
# CMakeLists). YM2151 renders at 62500 Hz and needs YM2203-class headroom
# (320 MHz / 1.15 V) to sustain it on FM-dense material -- 250 MHz fell behind.
target_compile_definitions(slave_ym2151 PRIVATE
    VGM_RP2350_SYSCLK_KHZ=${VGM_YM2151_SYSCLK_KHZ}
    VGM_RP2350_VREG_MV=${VGM_YM2151_VREG_MV}
)

# -DVGM_YM2151_USB_DIAG=ON : diagnostic build -- wait for a USB-CDC terminal at
# boot, print the system clock, and (pair with -DVGM_SLAVE_RATE_CHECK=ON) emit
# the once-per-second RATE CHECK line so the post-RESET sample rate is visible
# over USB. For chasing the "song plays ~2 semitones flat" (missed RESET) issue.
if(VGM_YM2151_USB_DIAG)
    target_compile_definitions(slave_ym2151 PRIVATE VGM_YM2151_USB_DIAG=1)
endif()
# -DVGM_YM2151_BOOT_DELAY_MS=N : hold off core1/SPI-RX start by N ms at boot
# (diagnostic; see src/main.c). Default 0.
if(DEFINED VGM_YM2151_BOOT_DELAY_MS)
    target_compile_definitions(slave_ym2151 PRIVATE VGM_YM2151_BOOT_DELAY_MS=${VGM_YM2151_BOOT_DELAY_MS})
endif()
