# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(AS7331_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/as7331)

zephyr_library()
zephyr_library_sources(as7331.c ${AS7331_DIR}/as7331_reg.c)
zephyr_library_include_directories(${AS7331_DIR})
