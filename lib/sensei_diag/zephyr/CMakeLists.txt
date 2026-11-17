# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_DIAG_CODEC is set.
set(SENSEI_DIAG_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_diag_codec)
zephyr_library_sources(${SENSEI_DIAG_LIB_DIR}/src/sensei_diag.c)
zephyr_include_directories(${SENSEI_DIAG_LIB_DIR}/include)
