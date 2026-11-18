# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds (the Zephyr library is in zephyr/): uplink queues, scheduling over radios and
# the receive side of a downlink. No Zephyr.
#   add_subdirectory(<sdk>/lib/sensei_uplink sensei_uplink)
#   target_link_libraries(<target> PRIVATE sensei_uplink)
# The buffer sizes are macros of include/sensei_uplink/uplink_sched.h (US_MAX_MSG, US_PREPARE_ROOM, US_MAX_MTU,
# US_MAX_RADIOS): set them with target_compile_definitions(sensei_uplink PUBLIC CONFIG_SENSEI_UPLINK_MAX_MSG=...).
# Or take the source lists from sensei_uplink.cmake (SENSEI_UPLINK_SOURCES, SENSEI_UPLINK_INCLUDE_DIRS).
# Host tests: cmake -S <sdk>/lib/sensei_uplink/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET sensei_link)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../sensei_link sensei_link)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/sensei_uplink.cmake)

add_library(sensei_uplink STATIC ${SENSEI_UPLINK_SOURCES})
target_include_directories(sensei_uplink PUBLIC ${SENSEI_UPLINK_INCLUDE_DIRS})
target_link_libraries(sensei_uplink PUBLIC sensei_link)
