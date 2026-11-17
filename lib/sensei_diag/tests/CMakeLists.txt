# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the diagnostics codec:
#   cmake -S lib/sensei_diag/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_diag_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_diag)

enable_testing()

add_executable(test_diag test_diag.c)
target_link_libraries(test_diag PRIVATE sensei_diag)
target_compile_options(test_diag PRIVATE -Wall -Wextra -Werror)
add_test(NAME diag COMMAND test_diag)
