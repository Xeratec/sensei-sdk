# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library: the arbitration logic only (the Zephyr wrapper is in zephyr/):
#   add_subdirectory(<sdk>/lib/sensei_busarb sensei_busarb)
#   target_link_libraries(<target> PRIVATE sensei_busarb)
# Host tests: cmake -S <sdk>/lib/sensei_busarb/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

add_library(sensei_busarb STATIC ${CMAKE_CURRENT_LIST_DIR}/src/bus_arb.c)
target_include_directories(sensei_busarb PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
