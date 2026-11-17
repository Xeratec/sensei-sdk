# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# GAP9 side of the link to the nRF5340, for GAP9 apps (they build with the GAP SDK, so the sources go into
# the app's own target and get its include paths):
#
#   include($ENV{SENSEI_SDK_ROOT}/lib/sensei_gap9/sensei_gap9.cmake)
#   target_sources(app PRIVATE ${SENSEI_GAP9_SIDE_SOURCES})
#   target_include_directories(app PRIVATE ${SENSEI_GAP9_SIDE_INCLUDE_DIRS})
#
# It also needs the sensei_link sources (sensei_link.cmake). The nRF5340 side is the Zephyr library
# (CONFIG_SENSEI_GAP9).

set(SENSEI_GAP9_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SENSEI_GAP9_SIDE_SOURCES ${SENSEI_GAP9_DIR}/gap9/gap9_side.c)
set(SENSEI_GAP9_SIDE_INCLUDE_DIRS ${SENSEI_GAP9_DIR}/include)
