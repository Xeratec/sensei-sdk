# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# SENSEI SSBL (see README.md). Build in the GAP SDK environment:
#   cmake -B build . [-DSENSEI_PLATFORM=gvsoc] && cmake --build build           (the ELF is build/ssbl)
# GVSoC test: scripts in test/ (tools/gvsoc_test.sh).
#   -DSENSEI_SSBL_FORCE_SLOT=0|1   boot that slot, do not read pad 56
#   -DSENSEI_SSBL_SLOT_A=<file> -DSENSEI_SSBL_SLOT_B=<file>   slot images put into the MRAM image of run/flash

cmake_minimum_required(VERSION 3.19)

set(SENSEI_PLATFORM "board" CACHE STRING "board or gvsoc (the simulator)")
set(SENSEI_SSBL_FORCE_SLOT "" CACHE STRING "0 or 1: boot that slot and ignore pad 56 (tests)")
set(SENSEI_SSBL_SLOT_A "" CACHE FILEPATH "slot A image for the MRAM image (tests, factory flash)")
set(SENSEI_SSBL_SLOT_B "" CACHE FILEPATH "slot B image for the MRAM image")
set(TARGET_NAME "ssbl")

set(ENV{KCONFIG_CONFIG} "config/sdk_${SENSEI_PLATFORM}.config")
set(SENSEI_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)

# Do NOT change the order of the next lines: the SDK's setup.cmake comes first.
include($ENV{GAP_SDK_HOME}/utils/cmake/setup.cmake)
project(${TARGET_NAME} C ASM)

add_executable(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/ssbl.c)
target_include_directories(${TARGET_NAME} PRIVATE ${SENSEI_SDK_DIR}/lib/sensei_gap9/include)
target_compile_options(${TARGET_NAME} PRIVATE -Os -std=gnu99)
target_link_options(${TARGET_NAME} PRIVATE -Os -flto)
if(NOT "${SENSEI_SSBL_FORCE_SLOT}" STREQUAL "")
    target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_FORCE_SLOT=${SENSEI_SSBL_FORCE_SLOT})
endif()

# The MRAM image: FSBL, this SSBL, the slots (the layout is ../layouts/sensei_ab.json).
list(APPEND GAPY_RUNNER_ARGS
    --flash-property=${CMAKE_BINARY_DIR}/ssbl@mram:ssbl:binary
    --flash-property=${CONFIG_GAP_SDK_HOME}/install/target/bin/fsbl@mram:fsbl:binary)
if(SENSEI_SSBL_SLOT_A)
    list(APPEND GAPY_RUNNER_ARGS --flash-property=${SENSEI_SSBL_SLOT_A}@mram:slot_a:file)
endif()
if(SENSEI_SSBL_SLOT_B)
    list(APPEND GAPY_RUNNER_ARGS --flash-property=${SENSEI_SSBL_SLOT_B}@mram:slot_b:file)
endif()

setupos(${TARGET_NAME})
