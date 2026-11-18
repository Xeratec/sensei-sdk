# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_UPLINK is set.
include(${CMAKE_CURRENT_LIST_DIR}/../sensei_uplink.cmake)

zephyr_library_named(sensei_uplink)
zephyr_library_sources(${SENSEI_UPLINK_SOURCES})
zephyr_include_directories(${SENSEI_UPLINK_INCLUDE_DIRS})
