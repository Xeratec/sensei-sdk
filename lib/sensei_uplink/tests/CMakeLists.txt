# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Host tests of the uplink scheduler (fake radios and clock), the message queue and the downlink receiver. The
# scheduler tests use the largest message and the node's room for the prepare hook: the defaults of uplink_sched.h.
#   cmake -S lib/sensei_uplink/tests -B <builddir> && cmake --build <builddir> && ctest --test-dir <builddir>
cmake_minimum_required(VERSION 3.16)
project(sensei_uplink_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. sensei_uplink)
target_compile_options(sensei_uplink PRIVATE -Wall -Wextra -Wtype-limits -Werror)

enable_testing()
foreach(t test_queue test_sched test_dl_rx)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE sensei_uplink)
  # The fragment vectors of sensei_link (generated/vectors.h)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../sensei_link/tests)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wtype-limits -Werror)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
