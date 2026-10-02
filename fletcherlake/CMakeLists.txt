cmake_minimum_required(VERSION 3.13)

list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

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

option(VCORE_SWEEP_WRITES_ENABLED
       "Compile voltage-changing commands into vcore_sweep" OFF)
set(VCORE0_SAFE_MIN_MV "" CACHE STRING "Approved VCORE0 minimum in mV")
set(VCORE0_SAFE_MAX_MV "" CACHE STRING "Approved VCORE0 maximum in mV")
set(VCORE1_SAFE_MIN_MV "" CACHE STRING "Approved VCORE1 minimum in mV")
set(VCORE1_SAFE_MAX_MV "" CACHE STRING "Approved VCORE1 maximum in mV")
set(VCORE2_SAFE_MIN_MV "" CACHE STRING "Approved VCORE2 minimum in mV")
set(VCORE2_SAFE_MAX_MV "" CACHE STRING "Approved VCORE2 maximum in mV")

if(VCORE_SWEEP_WRITES_ENABLED)
    foreach(limit
            VCORE0_SAFE_MIN_MV VCORE0_SAFE_MAX_MV
            VCORE1_SAFE_MIN_MV VCORE1_SAFE_MAX_MV
            VCORE2_SAFE_MIN_MV VCORE2_SAFE_MAX_MV)
        if(NOT "${${limit}}" STREQUAL "" AND
           NOT "${${limit}}" MATCHES "^[0-9]+$")
            message(FATAL_ERROR "${limit} must be an integer number of mV")
        endif()
    endforeach()

    foreach(rail VCORE0 VCORE1 VCORE2)
        if(NOT "${${rail}_SAFE_MIN_MV}" STREQUAL "" AND
           NOT "${${rail}_SAFE_MAX_MV}" STREQUAL "" AND
           ${rail}_SAFE_MIN_MV GREATER ${rail}_SAFE_MAX_MV)
            message(FATAL_ERROR
                    "${rail}_SAFE_MIN_MV must not exceed ${rail}_SAFE_MAX_MV")
        endif()
    endforeach()
endif()

add_executable(vcore_sweep
    vcore_sweep.c
)

target_link_libraries(vcore_sweep
    hardware_i2c
    pico_stdlib
)

target_compile_options(vcore_sweep PRIVATE -Wall -Wextra -Wformat=2)

if(VCORE_SWEEP_WRITES_ENABLED)
    target_compile_definitions(vcore_sweep PRIVATE
        VCORE_SWEEP_WRITES_ENABLED=1
    )
    foreach(limit
            VCORE0_SAFE_MIN_MV VCORE0_SAFE_MAX_MV
            VCORE1_SAFE_MIN_MV VCORE1_SAFE_MAX_MV
            VCORE2_SAFE_MIN_MV VCORE2_SAFE_MAX_MV)
        if(NOT "${${limit}}" STREQUAL "")
            target_compile_definitions(vcore_sweep PRIVATE
                ${limit}=${${limit}}
            )
        endif()
    endforeach()
endif()

pico_enable_stdio_usb(vcore_sweep 1)
pico_enable_stdio_uart(vcore_sweep 0)
pico_add_extra_outputs(vcore_sweep)

add_executable(fletcherlake_diag
    fletcherlake_diag.c
    serialtl_phy.c
)

pico_generate_pio_header(fletcherlake_diag
    ${CMAKE_CURRENT_LIST_DIR}/serialtl_phy.pio
)

target_link_libraries(fletcherlake_diag
    hardware_pio
    pico_stdlib
)

target_compile_options(fletcherlake_diag PRIVATE -Wall -Wextra -Wformat=2)
pico_enable_stdio_usb(fletcherlake_diag 1)
pico_enable_stdio_uart(fletcherlake_diag 0)
pico_add_extra_outputs(fletcherlake_diag)
