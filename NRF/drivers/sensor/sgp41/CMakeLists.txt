# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(SENSIRION_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/sensirion)

zephyr_library()
zephyr_library_sources(sgp41.c ${SENSIRION_DIR}/sgp41/sgp41_i2c.c)
zephyr_include_directories(${SENSIRION_DIR}/sgp41)
