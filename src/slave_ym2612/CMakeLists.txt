set(YMFM_DIR ${CMAKE_SOURCE_DIR}/third_party/ymfm)

add_executable(slave_ym2612
    src/main.c
    src/chip_ym2612.cpp
    ${YMFM_DIR}/src/ymfm_opn.cpp
    ${YMFM_DIR}/src/ymfm_adpcm.cpp
    ${YMFM_DIR}/src/ymfm_ssg.cpp
)

target_include_directories(slave_ym2612 PRIVATE ${YMFM_DIR}/src)

target_link_libraries(slave_ym2612 PRIVATE slave_common)

pico_enable_stdio_uart(slave_ym2612 1)
pico_enable_stdio_usb(slave_ym2612 1)

pico_add_extra_outputs(slave_ym2612)

# RP2350/Pico 2 overclock (see slave_common/include/slave_overclock.h and root CMakeLists).
target_compile_definitions(slave_ym2612 PRIVATE
    VGM_RP2350_SYSCLK_KHZ=${VGM_RP2350_SYSCLK_KHZ}
    VGM_RP2350_VREG_MV=${VGM_RP2350_VREG_MV}
)

# Mega Drive PCM data-bank cap (VGM 0x67 type-0). Default 192 KB (see
# chip_ym2612.h). Raise it on Pico 2 (RP2350, 520 KB RAM) for voice-heavy
# banks; lower it if an RP2040 build runs out of RAM.
if(DEFINED VGM_YM2612_PCM_BUDGET)
    target_compile_definitions(slave_ym2612 PRIVATE YM2612_PCM_BUDGET=${VGM_YM2612_PCM_BUDGET})
endif()
