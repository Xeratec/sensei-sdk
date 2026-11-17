# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# The SDK system code needs the board devices i2ca and an ADC channel: emulators on native_sim.
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_crypto_test)

# The known-answer tests are those of the example crypto_api (same file, no copy).
set(EXAMPLE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../example/crypto_api/src_NRF)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${EXAMPLE_DIR})
