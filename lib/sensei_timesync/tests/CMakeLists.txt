# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the time synchronization core:
#   cmake -S lib/sensei_timesync/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_timesync_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_timesync)
target_compile_options(sensei_timesync PRIVATE -Wall -Wextra -Werror)

enable_testing()
foreach(t model pair msg sim)
  add_executable(test_ts_${t} test_ts_${t}.c)
  target_link_libraries(test_ts_${t} PRIVATE sensei_timesync m)
  target_compile_options(test_ts_${t} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ts_${t} COMMAND test_ts_${t})
endforeach()
