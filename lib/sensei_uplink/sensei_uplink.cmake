# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Source and include lists for projects that compile the library into a target of their own, for example one that
# already builds the sensei_link sources:
#   include(<sdk>/lib/sensei_uplink/sensei_uplink.cmake)
#   target_sources(app PRIVATE ${SENSEI_UPLINK_SOURCES})
#   target_include_directories(app PRIVATE ${SENSEI_UPLINK_INCLUDE_DIRS})
# The sources need sensei_link (frag.h, sensei/types.pb.h) in the same target or a linked one.

get_filename_component(SENSEI_UPLINK_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

set(SENSEI_UPLINK_SOURCES
  ${SENSEI_UPLINK_DIR}/src/msg_queue.c
  ${SENSEI_UPLINK_DIR}/src/uplink_sched.c
  ${SENSEI_UPLINK_DIR}/src/dl_rx.c)
set(SENSEI_UPLINK_INCLUDE_DIRS ${SENSEI_UPLINK_DIR}/include)
