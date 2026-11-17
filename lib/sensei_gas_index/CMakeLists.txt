# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds (the Zephyr library is in zephyr/, GAP9 projects add the two sources):
#   add_subdirectory(<sdk>/lib/sensei_gas_index sensei_gas_index)
#   target_link_libraries(<target> PRIVATE sensei_gas_index)
# Host tests: cmake -S <sdk>/lib/sensei_gas_index/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

set(SENSEI_GAS_INDEX_VENDOR ${CMAKE_CURRENT_LIST_DIR}/third_party/sensirion_gas_index_algorithm)

add_library(sensei_gas_index STATIC
    ${CMAKE_CURRENT_LIST_DIR}/src/sensei_gas_index.c
    ${SENSEI_GAS_INDEX_VENDOR}/sensirion_gas_index_algorithm.c)
target_include_directories(sensei_gas_index PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${SENSEI_GAS_INDEX_VENDOR})
target_link_libraries(sensei_gas_index PUBLIC m)
