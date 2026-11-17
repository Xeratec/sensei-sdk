# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Added by NRF/CMakeLists.txt when CONFIG_SENSEI_GAS_INDEX is set.
set(SENSEI_GAS_INDEX_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(SENSEI_GAS_INDEX_VENDOR ${SENSEI_GAS_INDEX_DIR}/third_party/sensirion_gas_index_algorithm)

zephyr_library_named(sensei_gas_index)
zephyr_library_sources(
  ${SENSEI_GAS_INDEX_DIR}/src/sensei_gas_index.c
  ${SENSEI_GAS_INDEX_VENDOR}/sensirion_gas_index_algorithm.c
)
zephyr_include_directories(${SENSEI_GAS_INDEX_DIR}/include ${SENSEI_GAS_INDEX_VENDOR})
# libm of the toolchain libc. Not with the native libc (native_sim): the host libm.a of an x86_64 Linux needs the
# loader's CPU feature symbols and does not link there; the host libc brings its libm anyway.
if(NOT CONFIG_NATIVE_LIBC)
  zephyr_library_link_libraries(m)
endif()
