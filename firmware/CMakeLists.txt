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
    src/vdp_select.c
    ${CORE_DIR}/v9990.c
    ${CORE_DIR}/v9990_cmd.c
    ${CORE_DIR}/v9990_render.c
    ${CORE_DIR}/v9968.c
    ${CORE_DIR}/v9968_cmd.c
    ${CORE_DIR}/v9968_render.c
    ${CORE_DIR}/geo3d.c
)

pico_generate_pio_header(msx_v9990 ${CMAKE_CURRENT_LIST_DIR}/src/bus.pio)

target_include_directories(msx_v9990 PRIVATE src ${CORE_DIR})
target_compile_options(msx_v9990 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

set(V9990_PSRAM_CS_PIN "" CACHE STRING "PSRAM CS GPIO for boards other than the Pico Plus 2")
if(V9990_PSRAM_CS_PIN)
    target_compile_definitions(msx_v9990 PRIVATE V9990_PSRAM_CS_PIN=${V9990_PSRAM_CS_PIN})
endif()

# Boot trace: wait for the USB terminal, then print each boot step
option(MSX_BOOT_TRACE "Wait for USB serial and trace the boot steps" OFF)
if(MSX_BOOT_TRACE)
    target_compile_definitions(msx_v9990 PRIVATE MSX_BOOT_TRACE=1)
endif()

# VDP emulated when no choice was saved in flash (USB serial: chip v9990|v9968)
set(MSX_VDP_DEFAULT "V9990" CACHE STRING "Default VDP: V9990 or V9968")
set_property(CACHE MSX_VDP_DEFAULT PROPERTY STRINGS V9990 V9968)
if(MSX_VDP_DEFAULT STREQUAL "V9968")
    target_compile_definitions(msx_v9990 PRIVATE MSX_VDP_DEFAULT_V9968=1)
elseif(NOT MSX_VDP_DEFAULT STREQUAL "V9990")
    message(FATAL_ERROR "MSX_VDP_DEFAULT must be V9990 or V9968")
endif()

# PCB rev 0.2: the VDP comes from switch SW1 (GP22). Rev 0.1 (no switch,
# GP22 = /WR): -DMSX_VDP_SWITCH=OFF selects with MSX_VDP_DEFAULT / USB serial.
option(MSX_VDP_SWITCH "Select the VDP with the cartridge switch (PCB rev 0.2)" ON)
if(MSX_VDP_SWITCH)
    target_compile_definitions(msx_v9990 PRIVATE MSX_VDP_SWITCH=1)
else()
    target_compile_definitions(msx_v9990 PRIVATE MSX_VDP_SWITCH=0)
endif()

target_link_libraries(msx_v9990
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_flash
    hardware_watchdog
)

# USB CDC for debug output; UART0 pins are used by the MSX bus.
pico_enable_stdio_usb(msx_v9990 1)
pico_enable_stdio_uart(msx_v9990 0)

pico_add_extra_outputs(msx_v9990)
