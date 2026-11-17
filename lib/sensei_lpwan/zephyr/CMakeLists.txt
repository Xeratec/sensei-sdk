# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_LPWAN_PROTO is set (also by CONFIG_SENSEI_LPWAN).
set(SENSEI_LPWAN_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_lpwan)
zephyr_library_sources(
  ${SENSEI_LPWAN_DIR}/src/lpwan_sm.c
  ${SENSEI_LPWAN_DIR}/src/link_slave.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_timers.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_LPWAN
  ${SENSEI_LPWAN_DIR}/src/lpwan_master.c
  ${SENSEI_LPWAN_DIR}/zephyr/lpwan_client.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_LPWAN_SHELL ${SENSEI_LPWAN_DIR}/zephyr/lpwan_shell.c)
zephyr_include_directories(${SENSEI_LPWAN_DIR}/include)
zephyr_library_include_directories(${SENSEI_LPWAN_DIR}/zephyr)
