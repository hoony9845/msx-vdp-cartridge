cmake_minimum_required(VERSION 3.13)

# Defaults for the Pimoroni Pico Plus 2; override on the command line, e.g.
#   cmake -DPICO_BOARD=waveshare_core2350b ...        (Waveshare Core2350B1/B2)
#   cmake -DPICO_BOARD=<board> -DV9990_PSRAM_CS_PIN=<gpio> ...
set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(msx_v9990 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../core)

add_executable(msx_v9990
    src/main.c
    src/bus.c
    src/dvi_out.c
    src/psram.c
    ${CORE_DIR}/v9990.c
    ${CORE_DIR}/v9990_cmd.c
    ${CORE_DIR}/v9990_render.c
)

pico_generate_pio_header(msx_v9990 ${CMAKE_CURRENT_LIST_DIR}/src/bus.pio)

target_include_directories(msx_v9990 PRIVATE src ${CORE_DIR})
target_compile_options(msx_v9990 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

set(V9990_PSRAM_CS_PIN "" CACHE STRING "PSRAM CS GPIO for boards other than the Pico Plus 2")
if(V9990_PSRAM_CS_PIN)
    target_compile_definitions(msx_v9990 PRIVATE V9990_PSRAM_CS_PIN=${V9990_PSRAM_CS_PIN})
endif()

target_link_libraries(msx_v9990
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
)

# USB CDC for debug output; UART0 pins are used by the MSX bus.
pico_enable_stdio_usb(msx_v9990 1)
pico_enable_stdio_uart(msx_v9990 0)

pico_add_extra_outputs(msx_v9990)
