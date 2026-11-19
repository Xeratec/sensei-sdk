# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# The SDK system code needs the board devices i2ca and an ADC channel: emulators on native_sim.
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_secure_test)

# The tests are those of the host build (lib/sensei_secure/tests, same file and vectors), here with the PSA backend.
set(SECURE_TESTS ${CMAKE_CURRENT_LIST_DIR}/../../../lib/sensei_secure/tests)
target_sources(app PRIVATE src/main.c src/ratchet.c src/e2e.c)
target_include_directories(app PRIVATE ${SECURE_TESTS} ${SECURE_TESTS}/generated)
