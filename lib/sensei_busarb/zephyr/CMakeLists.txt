# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_BUSARB is set.
set(SENSEI_BUSARB_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_busarb)
zephyr_library_sources(
  ${SENSEI_BUSARB_DIR}/src/bus_arb.c
  ${SENSEI_BUSARB_DIR}/zephyr/bus_arb_zephyr.c)
zephyr_include_directories(${SENSEI_BUSARB_DIR}/include)
