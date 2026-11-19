# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library: the portable time synchronization logic (clock model, anchor pairing, wire format).
# The Zephyr/nRF5340 layer is in zephyr/:
#   add_subdirectory(<sdk>/lib/sensei_timesync sensei_timesync)
#   target_link_libraries(<target> PRIVATE sensei_timesync)
# Host tests: cmake -S <sdk>/lib/sensei_timesync/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

add_library(sensei_timesync STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/ts_model.c
  ${CMAKE_CURRENT_LIST_DIR}/src/ts_pair.c
  ${CMAKE_CURRENT_LIST_DIR}/src/ts_msg.c)
target_include_directories(sensei_timesync PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
if(NOT WIN32)
  target_link_libraries(sensei_timesync PUBLIC m) # sqrt, sqrtf
endif()
