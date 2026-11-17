# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_LINK is set. The nanopb runtime comes from the NCS module.
set(SENSEI_LINK_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_link)
zephyr_library_sources(
  ${SENSEI_LINK_DIR}/src/crc.c
  ${SENSEI_LINK_DIR}/src/sha256.c
  ${SENSEI_LINK_DIR}/src/cobs.c
  ${SENSEI_LINK_DIR}/src/stream.c
  ${SENSEI_LINK_DIR}/src/spi.c
  ${SENSEI_LINK_DIR}/src/frag.c
  ${SENSEI_LINK_DIR}/src/pb.c
  ${SENSEI_LINK_DIR}/generated/sensei/types.pb.c
  ${SENSEI_LINK_DIR}/generated/sensei/lpwan.pb.c)
zephyr_include_directories(${SENSEI_LINK_DIR}/include ${SENSEI_LINK_DIR}/generated)
