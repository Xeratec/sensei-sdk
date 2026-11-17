# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_GAP9 is set.
set(SENSEI_GAP9_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_gap9)
zephyr_library_sources(
  ${SENSEI_GAP9_DIR}/src/gap9_core.c
  ${SENSEI_GAP9_DIR}/zephyr/gap9_link.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_GAP9_SHELL ${SENSEI_GAP9_DIR}/zephyr/gap9_shell.c)
zephyr_include_directories(${SENSEI_GAP9_DIR}/include)
zephyr_library_include_directories(${SENSEI_GAP9_DIR}/zephyr)
# "pwr/pwr.h" (gap9_pwr) of the SDK system module
zephyr_library_include_directories(${SENSEI_GAP9_DIR}/../../NRF/system)
# The core sizes its queue at compile time (global, so that every user of gap9_core.h sees the same struct).
zephyr_compile_definitions(G9_QUEUE_DEPTH=${CONFIG_SENSEI_GAP9_QUEUE_DEPTH})
