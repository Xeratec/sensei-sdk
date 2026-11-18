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
#   -DSENSEI_SSBL_BOOT_ONLY=ON   (board) flash only the boot area: FSBL, SSBL, tables; the slots stay
#   -DSENSEI_SSBL_RECOVERY=ON -DSENSEI_SSBL_KIND=<board id>   recovery: with no bootable slot the SSBL runs the update
#       writer and serves it to the nRF5340 over SPI (needs -DSENSEI_SSBL_KIND, e.g. sensei-ieq-gap9; OFF by default: the
#       SSBL is then as before, no SPI is touched). SENSEI_SSBL_STRAP=ON: pad 57 high forces it (untested on hardware).
#       SENSEI_SSBL_RECOVERY_TEST=ON: GVSoC test build (jobs from the keep slot, test/recovery_feed.c), with
#       SENSEI_SSBL_RECOVERY_TEST_STRAP (strap high) or SENSEI_SSBL_RECOVERY_TEST_NOSEAL (no seal) for the cases.

cmake_minimum_required(VERSION 3.19)

set(SENSEI_PLATFORM "board" CACHE STRING "board or gvsoc (the simulator)")
set(SENSEI_SSBL_FORCE_SLOT "" CACHE STRING "0 or 1: boot that slot and ignore pad 56 (tests)")
set(SENSEI_SSBL_SLOT_A "" CACHE FILEPATH "slot A image for the MRAM image (tests, factory flash)")
set(SENSEI_SSBL_SLOT_B "" CACHE FILEPATH "slot B image for the MRAM image")
option(SENSEI_SSBL_RECOVERY "With no bootable slot run the update writer for the nRF5340 (OTA.md, Recovery)" OFF)
set(SENSEI_SSBL_KIND "" CACHE STRING "board id of the images (the kind TLV): needed for the recovery")
option(SENSEI_SSBL_STRAP "Recovery strap: pad 57 high forces recovery (UNVERIFIED hardware)" OFF)
option(SENSEI_SSBL_RECOVERY_TEST "GVSoC test build of the recovery: jobs from the keep slot, no SPI" OFF)
option(SENSEI_SSBL_RECOVERY_TEST_STRAP "Test build: the strap is high" OFF)
option(SENSEI_SSBL_RECOVERY_TEST_NOSEAL "Test build: the feed stops before the seal" OFF)
set(TARGET_NAME "ssbl")

# SENSEI_SSBL_BOOT_ONLY: the layout of the boot area alone (FSBL, SSBL, tables, no checkpoint and no slots): `flash` then
# writes the first 64 KiB of the MRAM and leaves the slots as they are (board build).
option(SENSEI_SSBL_BOOT_ONLY "board build: flash only the boot area, keep the slots" OFF)
set(_ssbl_config "sdk_${SENSEI_PLATFORM}")
if(SENSEI_SSBL_BOOT_ONLY)
    set(_ssbl_config "${_ssbl_config}_boot")
endif()
set(ENV{KCONFIG_CONFIG} "config/${_ssbl_config}.config")
set(SENSEI_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)

# pi_app_bin_load() skips the CRC32 check of the sections without CONFIG_DRIVER_CRC32 (gap-sdk bsp/partition/app_bin.c):
# the SSBL would start a damaged slot. The config file must say y.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/config/${_ssbl_config}.config" _ssbl_crc REGEX "^CONFIG_DRIVER_CRC32=y$")
if(NOT _ssbl_crc)
    message(FATAL_ERROR "config/${_ssbl_config}.config needs CONFIG_DRIVER_CRC32=y: without it the SSBL does not "
                        "check the CRC32 of the sections of a slot")
endif()

# Do NOT change the order of the next lines: the SDK's setup.cmake comes first.
include($ENV{GAP_SDK_HOME}/utils/cmake/setup.cmake)
project(${TARGET_NAME} C ASM)

add_executable(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/ssbl.c)
target_include_directories(${TARGET_NAME} PRIVATE ${SENSEI_SDK_DIR}/lib/sensei_gap9/include)
target_compile_options(${TARGET_NAME} PRIVATE -Os -std=gnu99)
if(SENSEI_SSBL_RECOVERY)
    if("${SENSEI_SSBL_KIND}" STREQUAL "")
        message(FATAL_ERROR "SENSEI_SSBL_RECOVERY needs -DSENSEI_SSBL_KIND=<board id of the images>, for example "
                            "sensei-ieq-gap9: the writer refuses an image of another kind")
    endif()
    # The writer, the job server, the MRAM operations, the link side, the link layer (framing, SHA-256, nanopb, messages).
    include(${SENSEI_SDK_DIR}/lib/sensei_gap9/sensei_gap9.cmake)
    include(${SENSEI_SDK_DIR}/lib/sensei_link/sensei_link.cmake)
    set(_rec_sources ${CMAKE_CURRENT_LIST_DIR}/src/recovery.c ${SENSEI_GAP9_RECOVERY_SOURCES}
                     ${SENSEI_LINK_SOURCES})
    if(SENSEI_SSBL_RECOVERY_TEST)
        list(APPEND _rec_sources ${CMAKE_CURRENT_LIST_DIR}/test/recovery_feed.c)
    else()
        list(APPEND _rec_sources ${CMAKE_CURRENT_LIST_DIR}/src/recovery_link.c ${SENSEI_GAP9_SIDE_SOURCES})
    endif()
    target_sources(${TARGET_NAME} PRIVATE ${_rec_sources})
    target_include_directories(${TARGET_NAME} PRIVATE ${SENSEI_GAP9_OTA_INCLUDE_DIRS} ${SENSEI_LINK_INCLUDE_DIRS}
                               ${CMAKE_CURRENT_LIST_DIR}/src)
    target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_RECOVERY SENSEI_SSBL_KIND="${SENSEI_SSBL_KIND}"
                               ${SENSEI_LINK_DEFINITIONS})
    execute_process(COMMAND git -C ${SENSEI_SDK_DIR} rev-parse --short=8 HEAD OUTPUT_VARIABLE _ssbl_git
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_ssbl_git MATCHES "^[0-9a-f]+$")
        target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_GIT=0x${_ssbl_git}u)
    endif()
    if(SENSEI_SSBL_STRAP)
        target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_STRAP)
    endif()
    if(SENSEI_SSBL_RECOVERY_TEST)
        target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_RECOVERY_TEST)
        if(SENSEI_SSBL_RECOVERY_TEST_STRAP)
            target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_RECOVERY_TEST_STRAP)
        endif()
        if(SENSEI_SSBL_RECOVERY_TEST_NOSEAL)
            target_compile_definitions(${TARGET_NAME} PRIVATE SENSEI_SSBL_RECOVERY_TEST_NOSEAL)
        endif()
    endif()
endif()
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
