# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_CHARGE is set.
set(SENSEI_CHARGE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_charge)
zephyr_library_sources(${SENSEI_CHARGE_DIR}/src/sensei_charge.c)
zephyr_include_directories(${SENSEI_CHARGE_DIR}/include)
