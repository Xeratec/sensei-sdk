# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_TIMESYNC is set.
set(SENSEI_TIMESYNC_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_timesync)
zephyr_library_sources(
  ${SENSEI_TIMESYNC_DIR}/src/ts_model.c
  ${SENSEI_TIMESYNC_DIR}/src/ts_pair.c
  ${SENSEI_TIMESYNC_DIR}/src/ts_msg.c
  ${SENSEI_TIMESYNC_DIR}/zephyr/ts_timebase.c
  ${SENSEI_TIMESYNC_DIR}/zephyr/ts_capture.c
  ${SENSEI_TIMESYNC_DIR}/zephyr/ts_pps.c
  ${SENSEI_TIMESYNC_DIR}/zephyr/ts_core.c
  ${SENSEI_TIMESYNC_DIR}/zephyr/ts_ble.c)
zephyr_include_directories(${SENSEI_TIMESYNC_DIR}/include)
# The native simulator boards (BabbleSim) take the math library of the host in the final link.
if(NOT CONFIG_ARCH_POSIX)
  zephyr_library_link_libraries(m)
endif()
