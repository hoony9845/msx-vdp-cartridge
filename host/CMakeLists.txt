# Host (PC) build of the portable V9990 core: unit tests + render demo.
cmake_minimum_required(VERSION 3.13)
project(v9990_host C)

set(CMAKE_C_STANDARD 11)
set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../core)

add_library(v9990_core STATIC
    ${CORE_DIR}/v9990.c
    ${CORE_DIR}/v9990_cmd.c
    ${CORE_DIR}/v9990_render.c
)
target_include_directories(v9990_core PUBLIC ${CORE_DIR})
target_compile_options(v9990_core PRIVATE -Wall -Wextra -O2)

add_executable(test_v9990 test_v9990.c)
target_link_libraries(test_v9990 v9990_core)
target_compile_options(test_v9990 PRIVATE -Wall -Wextra)

add_executable(render_demo render_demo.c)
target_link_libraries(render_demo v9990_core)

enable_testing()
add_test(NAME test_v9990 COMMAND test_v9990)

add_executable(replay replay.c)
target_link_libraries(replay v9990_core)
target_compile_options(replay PRIVATE -Wall -Wextra)
