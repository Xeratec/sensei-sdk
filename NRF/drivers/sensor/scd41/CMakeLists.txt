# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(SENSIRION_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/sensirion)

zephyr_library()
zephyr_library_sources(scd41.c ${SENSIRION_DIR}/scd4x/scd4x_i2c.c)
zephyr_include_directories(${SENSIRION_DIR}/scd4x)
