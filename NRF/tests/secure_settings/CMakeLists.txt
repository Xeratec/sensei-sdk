# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# The SDK system code needs the board devices i2ca and an ADC channel: emulators on native_sim.
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_secure_settings_test)

# The key store of lib/sensei_secure on the settings subsystem (flash simulator of native_sim).
target_sources(app PRIVATE src/main.c)
# The vectors of the sealed settings records (vector 7), the same as in the host test of lib/sensei_secure.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/sensei_secure/tests/generated)
