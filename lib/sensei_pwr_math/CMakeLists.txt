# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds (the Zephyr library is in zephyr/):
#   add_subdirectory(<sdk>/lib/sensei_pwr_math sensei_pwr_math)
#   target_link_libraries(<target> PRIVATE sensei_pwr_math)
# Host tests: cmake -S <sdk>/lib/sensei_pwr_math/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

add_library(sensei_pwr_math STATIC ${CMAKE_CURRENT_LIST_DIR}/src/sensei_pwr_math.c)
target_include_directories(sensei_pwr_math PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
