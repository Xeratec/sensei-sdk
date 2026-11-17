# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds (the Zephyr library is in zephyr/, GAP9 projects use sensei_delta.cmake):
#   add_subdirectory(<sdk>/lib/sensei_delta sensei_delta)
#   target_link_libraries(<target> PRIVATE sensei_delta)
# Options (set before add_subdirectory):
#   SENSEI_DELTA_PORT     NRF5340, GAP9 or HOST: profiling hooks of include/sensei_delta_port.h
#   SENSEI_DELTA_HUFFMAN  ON (default) or OFF: build the Huffman coder
#   SENSEI_DELTA_RANS     ON (default) or OFF: build the rANS coder
# Host tests: cmake -S <sdk>/lib/sensei_delta/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

option(SENSEI_DELTA_HUFFMAN "Build the Huffman coder" ON)
option(SENSEI_DELTA_RANS "Build the rANS coder" ON)
if(NOT DEFINED SENSEI_DELTA_PORT)
    set(SENSEI_DELTA_PORT "" CACHE STRING "Profiling port: NRF5340, GAP9 or HOST")
endif()

add_library(sensei_delta STATIC ${CMAKE_CURRENT_LIST_DIR}/src/sensei_delta.c)
target_include_directories(sensei_delta PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
# The codec-unit conversion must be plain IEEE double operations.
target_compile_options(sensei_delta PRIVATE -ffp-contract=off)
target_compile_definitions(sensei_delta PUBLIC
    SENSEI_DELTA_ENABLE_HUFFMAN=$<BOOL:${SENSEI_DELTA_HUFFMAN}>
    SENSEI_DELTA_ENABLE_RANS=$<BOOL:${SENSEI_DELTA_RANS}>)
if(SENSEI_DELTA_PORT)
    target_compile_definitions(sensei_delta PUBLIC SENSEI_DELTA_PORT_${SENSEI_DELTA_PORT})
endif()
target_link_libraries(sensei_delta PUBLIC m)
