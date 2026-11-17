# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# Test app of the SSBL (GVSoC). The MRAM image of this project is the app binary alone (layout_app.json):
#   cmake -B build . -DAPP_ID=A [-DSENSEI_SDK_DIR=<sdk>] && cmake --build build && cmake --build build -t image   -> build/mram.bin
cmake_minimum_required(VERSION 3.19)
set(TARGET_NAME "ssbl_test_app")
set(APP_ID "A" CACHE STRING "A or B")
set(SENSEI_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." CACHE PATH "SENSEI SDK root (the test copies this folder)")

include($ENV{GAP_SDK_HOME}/utils/cmake/setup.cmake)
project(${TARGET_NAME} C ASM)
add_executable(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/app.c)
target_include_directories(${TARGET_NAME} PRIVATE ${SENSEI_SDK_DIR}/lib/sensei_gap9/include)
target_compile_definitions(${TARGET_NAME} PRIVATE APP_ID="${APP_ID}")
list(APPEND GAPY_RUNNER_ARGS --flash-property=${CMAKE_BINARY_DIR}/${TARGET_NAME}@mram:app:binary)
setupos(${TARGET_NAME})
