# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Plain CMake library for host and GAP9 builds (the Zephyr library is in zephyr/):
#   add_subdirectory(<sdk>/lib/sensei_link sensei_link)
#   target_link_libraries(<target> PRIVATE sensei_link)
# Or take the source lists from sensei_link.cmake (SENSEI_LINK_SOURCES, SENSEI_LINK_INCLUDE_DIRS).
# Host tests: cmake -S <sdk>/lib/sensei_link/tests -B <builddir>

cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/sensei_link.cmake)

add_library(sensei_link STATIC ${SENSEI_LINK_SOURCES})
target_include_directories(sensei_link PUBLIC ${SENSEI_LINK_INCLUDE_DIRS})
target_compile_definitions(sensei_link PUBLIC ${SENSEI_LINK_DEFINITIONS})

# The framing sources alone, for users that need no protobuf (plain C99).
add_library(sensei_link_framing STATIC ${SENSEI_LINK_FRAMING_SOURCES})
target_include_directories(sensei_link_framing PUBLIC ${SENSEI_LINK_DIR}/include)
target_compile_definitions(sensei_link_framing PUBLIC ${SENSEI_LINK_DEFINITIONS})
