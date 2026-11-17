# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the power math:
#   cmake -S lib/sensei_pwr_math/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_pwr_math_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_pwr_math)

enable_testing()

add_executable(test_pwr_math test_pwr_math.c)
target_link_libraries(test_pwr_math PRIVATE sensei_pwr_math)
target_compile_options(test_pwr_math PRIVATE -Wall -Wextra -Werror)
add_test(NAME pwr_math COMMAND test_pwr_math)
