# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# Setup SENSEI SDK
set(SENSEI_SDK_ROOT $ENV{SENSEI_SDK_ROOT})
if(NOT SENSEI_SDK_ROOT)
    message(FATAL_ERROR "SENSEI_SDK_ROOT is not defined")
endif()

list(APPEND EXTRA_ZEPHYR_MODULES
    ${SENSEI_SDK_ROOT}/NRF
)

# Configure the project
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_timesync)

target_sources(app PRIVATE
    src/main.c
    src/report.c
)
target_sources_ifdef(CONFIG_SENSEI_TIMESYNC_MASTER app PRIVATE
    src/master.c
    src/measure.c
)
target_sources_ifdef(CONFIG_SENSEI_TIMESYNC_FOLLOWER app PRIVATE
    src/follower.c
)

target_include_directories(app PRIVATE
    src
)
