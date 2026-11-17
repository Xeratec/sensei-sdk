# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host builds (the Zephyr library is in zephyr/):
#   add_subdirectory(<sdk>/lib/sensei_diag sensei_diag)
#   target_link_libraries(<target> PRIVATE sensei_diag)
# Host tests: cmake -S <sdk>/lib/sensei_diag/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

add_library(sensei_diag STATIC ${CMAKE_CURRENT_LIST_DIR}/src/sensei_diag.c)
target_include_directories(sensei_diag PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
