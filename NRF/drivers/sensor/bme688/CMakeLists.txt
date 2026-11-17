# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(BOSCH_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/bosch/bme68x)

zephyr_library()
zephyr_library_sources(bme688.c ${BOSCH_DIR}/bme68x.c)
zephyr_include_directories(${BOSCH_DIR})
