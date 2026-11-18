# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the charge counter:
#   cmake -S lib/sensei_charge/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_charge_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_charge)

enable_testing()

add_executable(test_charge test_charge.c)
target_link_libraries(test_charge PRIVATE sensei_charge)
target_compile_options(test_charge PRIVATE -Wall -Wextra -Werror)
add_test(NAME charge COMMAND test_charge)
