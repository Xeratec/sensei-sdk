# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds: the nRF5340 protocol state machine (the Zephyr part is in zephyr/,
# the GAP9 side in gap9/ with sensei_gap9.cmake):
#   add_subdirectory(<sdk>/lib/sensei_gap9 sensei_gap9)
#   target_link_libraries(<target> PRIVATE sensei_gap9_core)
# and the update of the GAP9 application (OTA.md): the writer that runs on GAP9 and the flow of the nRF5340,
# both without an OS:
#   target_link_libraries(<target> PRIVATE sensei_gap9_ota)
# Host tests: cmake -S <sdk>/lib/sensei_gap9/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET sensei_link)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../sensei_link sensei_link)
endif()

# G9_QUEUE_DEPTH (default 3) is the number of queued jobs.
add_library(sensei_gap9_core STATIC ${CMAKE_CURRENT_LIST_DIR}/src/gap9_core.c)
target_include_directories(sensei_gap9_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(sensei_gap9_core PUBLIC sensei_link)

# Update of the GAP9 application: the writer of GAP9 (g9ota.c) and the flow of the nRF5340 (g9ota_flow.c). They need
# the image helpers of NRF/system/ota (header only).
add_library(sensei_gap9_ota STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/g9ota.c
  ${CMAKE_CURRENT_LIST_DIR}/src/g9rec.c
  ${CMAKE_CURRENT_LIST_DIR}/src/g9rec_codec.c
  ${CMAKE_CURRENT_LIST_DIR}/src/g9ota_flow.c)
target_include_directories(sensei_gap9_ota PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../../NRF/system/ota)
target_link_libraries(sensei_gap9_ota PUBLIC sensei_link)
