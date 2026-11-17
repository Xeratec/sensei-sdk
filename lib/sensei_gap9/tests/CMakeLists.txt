# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host test of the GAP9 link state machine (nRF5340 side) against a simulated GAP9 master:
#   cmake -S lib/sensei_gap9/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_gap9_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_gap9)
target_compile_options(sensei_gap9_core PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_gap9_core test_gap9_core.c)
target_link_libraries(test_gap9_core PRIVATE sensei_gap9_core)
target_compile_options(test_gap9_core PRIVATE -Wall -Wextra -Werror)
add_test(NAME gap9_core COMMAND test_gap9_core)
