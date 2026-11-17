# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Sources of the SENSEI link layer for projects that build their own way (GAP9, host tools).
#
#   include($ENV{SENSEI_SDK_ROOT}/lib/sensei_link/sensei_link.cmake)
#   target_sources(app PRIVATE ${SENSEI_LINK_SOURCES})
#   target_include_directories(app PRIVATE ${SENSEI_LINK_INCLUDE_DIRS})
#
# The nanopb runtime comes from the submodule third_party/nanopb (the commit that NCS ships); set
# SENSEI_NANOPB_DIR to use another copy of the same version. The generated sources need C11 (anonymous
# unions) or GNU C99, for example -std=gnu99 on the GAP9 toolchain. On Zephyr use CONFIG_SENSEI_LINK instead.

set(SENSEI_LINK_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT SENSEI_NANOPB_DIR)
  set(SENSEI_NANOPB_DIR "${SENSEI_LINK_DIR}/third_party/nanopb")
endif()
if(NOT EXISTS "${SENSEI_NANOPB_DIR}/pb_encode.c")
  message(FATAL_ERROR "nanopb not found at ${SENSEI_NANOPB_DIR}: run 'git submodule update --init' in the SENSEI SDK")
endif()

# Framing only (plain C99, no nanopb).
set(SENSEI_LINK_FRAMING_SOURCES
  ${SENSEI_LINK_DIR}/src/crc.c
  ${SENSEI_LINK_DIR}/src/cobs.c
  ${SENSEI_LINK_DIR}/src/stream.c
  ${SENSEI_LINK_DIR}/src/spi.c
  ${SENSEI_LINK_DIR}/src/frag.c)

set(SENSEI_LINK_GENERATED_SOURCES
  ${SENSEI_LINK_DIR}/generated/sensei/types.pb.c
  ${SENSEI_LINK_DIR}/generated/sensei/lpwan.pb.c)

set(SENSEI_NANOPB_SOURCES
  ${SENSEI_NANOPB_DIR}/pb_common.c
  ${SENSEI_NANOPB_DIR}/pb_encode.c
  ${SENSEI_NANOPB_DIR}/pb_decode.c)

set(SENSEI_LINK_SOURCES
  ${SENSEI_LINK_FRAMING_SOURCES}
  ${SENSEI_LINK_DIR}/src/pb.c
  ${SENSEI_LINK_GENERATED_SOURCES}
  ${SENSEI_NANOPB_SOURCES})

set(SENSEI_LINK_INCLUDE_DIRS
  ${SENSEI_LINK_DIR}/include
  ${SENSEI_LINK_DIR}/generated
  ${SENSEI_NANOPB_DIR})
