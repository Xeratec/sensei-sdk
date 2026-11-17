# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubx_test)

# The UBX helpers have no Zephyr dependency, so the test compiles the source directly.
set(UBX_DIR ${CMAKE_CURRENT_LIST_DIR}/../../drivers/gnss)
target_sources(app PRIVATE src/main.c ${UBX_DIR}/ubx.c)
target_include_directories(app PRIVATE ${UBX_DIR})
