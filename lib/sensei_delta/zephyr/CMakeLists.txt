# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_DELTA is set.
set(SENSEI_DELTA_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

zephyr_library_named(sensei_delta)
zephyr_library_sources(${SENSEI_DELTA_DIR}/src/sensei_delta.c)
zephyr_include_directories(${SENSEI_DELTA_DIR}/include)
# The units formula needs plain IEEE double operations.
zephyr_library_compile_options(-ffp-contract=off)

# The library and the application (through the header) see the same macros.
zephyr_compile_definitions(
  SENSEI_DELTA_ENABLE_RANS=$<IF:$<BOOL:${CONFIG_SENSEI_DELTA_RANS}>,1,0>
  SENSEI_DELTA_ENABLE_HUFFMAN=$<IF:$<BOOL:${CONFIG_SENSEI_DELTA_HUFFMAN}>,1,0>
)

if(CONFIG_SENSEI_DELTA_PROFILE)
  zephyr_compile_definitions(SENSEI_DELTA_PORT_NRF5340)
endif()
