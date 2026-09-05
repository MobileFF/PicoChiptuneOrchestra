add_library(slave_common STATIC
    src/slave_spi_rx.c
    src/audio_pwm.c
    src/slave_engine.c
)

target_include_directories(slave_common PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/src/protocol
)

target_compile_definitions(slave_common PUBLIC
    VGM_SLAVE_VERBOSE_LOG=$<BOOL:${VGM_SLAVE_VERBOSE_LOG}>
    VGM_SLAVE_RATE_CHECK=$<BOOL:${VGM_SLAVE_RATE_CHECK}>
    VGM_SLAVE_RX_TRACE=$<BOOL:${VGM_SLAVE_RX_TRACE}>
)
# VGM_RP2350_SYSCLK_KHZ / VGM_RP2350_VREG_MV are set per-target (see each
# heavy slave's CMakeLists) because YM2203 needs a higher clock than the rest.

target_link_libraries(slave_common PUBLIC
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_pwm
    hardware_gpio
    hardware_clocks
    hardware_vreg
)
