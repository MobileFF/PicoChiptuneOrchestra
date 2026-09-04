add_executable(slave_sn76489
    src/main.c
    src/chip_sn76489.c
)

target_link_libraries(slave_sn76489 PRIVATE slave_common)

pico_enable_stdio_uart(slave_sn76489 1)
pico_enable_stdio_usb(slave_sn76489 1)

pico_add_extra_outputs(slave_sn76489)
