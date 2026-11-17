# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

set(BH1730_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/bh1730fvc)

zephyr_library()
zephyr_library_sources(bh1730.c ${BH1730_DIR}/bh1730fvc_reg.c)
zephyr_library_include_directories(${BH1730_DIR})
