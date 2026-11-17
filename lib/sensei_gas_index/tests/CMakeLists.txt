# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the gas index wrapper:
#   cmake -S lib/sensei_gas_index/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_gas_index_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_gas_index)

enable_testing()

add_executable(test_gas_index test_gas_index.c)
target_link_libraries(test_gas_index PRIVATE sensei_gas_index)
target_compile_options(test_gas_index PRIVATE -Wall -Wextra -Werror)
add_test(NAME gas_index COMMAND test_gas_index)
