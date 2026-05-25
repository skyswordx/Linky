cmake_minimum_required(VERSION 3.16)

project(Linky
    VERSION 0.1.0
    DESCRIPTION "Linux low-latency buffer and fd-passing experiments"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(LINKY_BUILD_TESTS "Build Linky self tests" ON)

add_library(linky_core
    src/buffer_pool.c
    src/ring.c
    src/runner.c
    src/selftest.c
    src/time_stats.c)

target_include_directories(linky_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(linky_core PUBLIC _GNU_SOURCE)

target_compile_options(linky_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wstrict-prototypes)

find_package(Threads REQUIRED)
target_link_libraries(linky_core PUBLIC Threads::Threads)

add_executable(linky src/main.c)
target_link_libraries(linky PRIVATE linky_core)
target_compile_options(linky PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wstrict-prototypes)

if(LINKY_BUILD_TESTS)
    enable_testing()
    add_executable(linky_selftest tests/test_selftest.c)
    target_link_libraries(linky_selftest PRIVATE linky_core)
    target_compile_options(linky_selftest PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wstrict-prototypes)
    add_test(NAME linky_selftest COMMAND linky_selftest)
endif()
