# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# The retained RAM driver and its binding come from the diag test
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../diag)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_ota_reset_test)

target_sources(app PRIVATE
  src/main.c
  ../diag/src/test_retained_mem.c
  ${CMAKE_CURRENT_LIST_DIR}/../../system/ota/sensei_ota_reset.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../system)
