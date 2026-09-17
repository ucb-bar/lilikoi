cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(hello C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(hello
    main.c
)

target_link_libraries(hello
    pico_stdlib
)

# Send printf() over USB
pico_enable_stdio_usb(hello 1)
pico_enable_stdio_uart(hello 0)

pico_add_extra_outputs(hello)

add_executable(i2c_check
    i2c_check.c
)

target_link_libraries(i2c_check
    hardware_i2c
    pico_stdlib
)

# Send printf() over USB
pico_enable_stdio_usb(i2c_check 1)
pico_enable_stdio_uart(i2c_check 0)

pico_add_extra_outputs(i2c_check)
