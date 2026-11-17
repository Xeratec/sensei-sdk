# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(ILPS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/st/ilps28qsw)

zephyr_library()
zephyr_library_sources(ilps28qsw.c ${ILPS_DIR}/ilps28qsw_reg.c)
zephyr_library_include_directories(${ILPS_DIR})
