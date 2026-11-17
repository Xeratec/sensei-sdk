# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the sensor math:
#   cmake -S lib/sensei_sensor_math/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_sensor_math_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_sensor_math)

enable_testing()

add_executable(test_sensor_math test_sensor_math.c)
target_link_libraries(test_sensor_math PRIVATE sensei_sensor_math)
target_compile_options(test_sensor_math PRIVATE -Wall -Wextra -Werror)
add_test(NAME sensor_math COMMAND test_sensor_math)
