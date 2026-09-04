set(YMFM_DIR ${CMAKE_SOURCE_DIR}/third_party/ymfm)

add_executable(slave_ym2413
    src/main.c
    src/chip_ym2413.cpp
    ${YMFM_DIR}/src/ymfm_opl.cpp
    ${YMFM_DIR}/src/ymfm_adpcm.cpp
    ${YMFM_DIR}/src/ymfm_pcm.cpp
)

target_include_directories(slave_ym2413 PRIVATE ${YMFM_DIR}/src)

target_link_libraries(slave_ym2413 PRIVATE slave_common)

pico_enable_stdio_uart(slave_ym2413 1)
pico_enable_stdio_usb(slave_ym2413 1)

pico_add_extra_outputs(slave_ym2413)
