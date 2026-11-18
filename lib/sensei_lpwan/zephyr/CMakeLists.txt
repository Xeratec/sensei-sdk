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
  ${SENSEI_LPWAN_DIR}/src/lpwan_timers.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_lora.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_dfu.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_upd.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_rai.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_counters.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_gnss.c
  ${SENSEI_LPWAN_DIR}/src/lpwan_shield.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_LPWAN
  ${SENSEI_LPWAN_DIR}/src/lpwan_master.c
  ${SENSEI_LPWAN_DIR}/zephyr/lpwan_client.c)
zephyr_library_sources_ifdef(CONFIG_SENSEI_LPWAN_SHELL ${SENSEI_LPWAN_DIR}/zephyr/lpwan_shell.c)
# Simulated shield link (native_sim): the client side, and the host sockets in the native simulator runner.
zephyr_library_sources_ifdef(CONFIG_SENSEI_LPWAN_SIM ${SENSEI_LPWAN_DIR}/zephyr/sim/lpwan_sim_master.c)
if(CONFIG_SENSEI_LPWAN_SIM_HOST)
  zephyr_library_sources(${SENSEI_LPWAN_DIR}/src/lpwan_sim.c)
  target_sources(native_simulator INTERFACE ${SENSEI_LPWAN_DIR}/zephyr/sim/lpwan_sim_host.c)
  target_compile_options(native_simulator BEFORE INTERFACE -I${SENSEI_LPWAN_DIR}/include)
  # cmdline.h of native_sim; the BabbleSim board has its own on the include path
  if(CONFIG_BOARD_NATIVE_SIM)
    zephyr_library_include_directories(${ZEPHYR_BASE}/boards/native/native_sim)
  endif()
endif()
zephyr_include_directories(${SENSEI_LPWAN_DIR}/include)
zephyr_library_include_directories(${SENSEI_LPWAN_DIR}/zephyr)
