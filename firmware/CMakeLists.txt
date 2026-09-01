cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD    pico2_w CACHE STRING "Board")
set(PICO_PLATFORM rp2350  CACHE STRING "Platform")

include(cmake/pico_sdk_import.cmake)
include(cmake/FreeRTOS_Kernel_import.cmake)

project(r5f_sim C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(r5f_sim
    src/main.c
    src/telemetry.c
    src/engine/engine.c
    src/engine/frame.c
    src/engine/ringbuf.c
    src/port/port_rp2350.c
)

target_include_directories(r5f_sim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}          # FreeRTOSConfig.h
)

target_link_libraries(r5f_sim PRIVATE
    pico_stdlib
    hardware_uart
    hardware_pwm
    hardware_gpio
    hardware_irq
    hardware_clocks
    hardware_sync
    FreeRTOS-Kernel-Heap4
)

target_compile_options(r5f_sim PRIVATE -Wall -Wextra -Werror)

# UART0 is the device under test. Console output must never touch it.
pico_enable_stdio_uart(r5f_sim 0)
pico_enable_stdio_usb(r5f_sim 1)

target_compile_definitions(r5f_sim PRIVATE
    # Do not wait for a host at boot: the board must run standalone.
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    # If the host stops reading, give up quickly rather than stalling the
    # telemetry task for half a second and distorting the CPU figures.
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=20000
    PICO_STDIO_USB_DEFAULT_CRLF=0
)

pico_add_extra_outputs(r5f_sim)
