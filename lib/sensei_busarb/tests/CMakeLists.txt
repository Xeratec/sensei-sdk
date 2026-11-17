# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host test of the bus arbiter:
#   cmake -S lib/sensei_busarb/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_busarb_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_busarb)
target_compile_options(sensei_busarb PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_bus_arb test_bus_arb.c)
target_link_libraries(test_bus_arb PRIVATE sensei_busarb)
target_compile_options(test_bus_arb PRIVATE -Wall -Wextra -Werror)
add_test(NAME bus_arb COMMAND test_bus_arb)
