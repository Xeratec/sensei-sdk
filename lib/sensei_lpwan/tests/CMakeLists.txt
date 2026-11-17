# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the nRF5340 to nRF9151 link: the master core and the slave core run against each other
# through a simulated SPI, with faults (corrupt frames, lost responses, full queues); and the 3GPP timer coding.
#   cmake -S lib/sensei_lpwan/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_lpwan_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_lpwan)
target_compile_options(sensei_lpwan PRIVATE -Wall -Wextra -Werror)

enable_testing()
foreach(t test_lpwan_link test_lpwan_timers)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE sensei_lpwan)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
