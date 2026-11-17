# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the delta coder engine (synthetic stand-in tables, not the trained ones):
#   cmake -S lib/sensei_delta/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_delta_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(SENSEI_DELTA_SANITIZE "ASan and UBSan in Debug builds" ON)

set(_warn -Wall -Wextra -Werror -ffp-contract=off)
set(_san "")
if(SENSEI_DELTA_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_san -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_delta)
target_compile_options(sensei_delta PRIVATE ${_warn} ${_san})
if(_san)
    target_link_options(sensei_delta PUBLIC ${_san})
endif()

enable_testing()

add_executable(test_roundtrip test_roundtrip.c generated/standin_tables.c)
target_include_directories(test_roundtrip PRIVATE ${CMAKE_CURRENT_LIST_DIR}/generated)
target_link_libraries(test_roundtrip PRIVATE sensei_delta)
target_compile_options(test_roundtrip PRIVATE ${_warn} ${_san})
add_test(NAME roundtrip COMMAND test_roundtrip)
