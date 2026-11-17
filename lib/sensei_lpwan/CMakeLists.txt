# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds: the protocol cores of both sides of the nRF5340 to nRF9151 link
# (no Zephyr). The Zephyr client is in zephyr/.
#   add_subdirectory(<sdk>/lib/sensei_lpwan sensei_lpwan)
#   target_link_libraries(<target> PRIVATE sensei_lpwan)
# Host tests: cmake -S <sdk>/lib/sensei_lpwan/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET sensei_link)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../sensei_link sensei_link)
endif()

add_library(sensei_lpwan STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/lpwan_sm.c
  ${CMAKE_CURRENT_LIST_DIR}/src/link_slave.c
  ${CMAKE_CURRENT_LIST_DIR}/src/lpwan_master.c
  ${CMAKE_CURRENT_LIST_DIR}/src/lpwan_timers.c
  ${CMAKE_CURRENT_LIST_DIR}/src/lpwan_lora.c
  ${CMAKE_CURRENT_LIST_DIR}/src/lpwan_dfu.c)
target_include_directories(sensei_lpwan PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(sensei_lpwan PUBLIC sensei_link)
