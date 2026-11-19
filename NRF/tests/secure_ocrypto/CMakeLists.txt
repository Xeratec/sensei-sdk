# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_secure_ocrypto)

# The SENSEI board enables the old USB device stack, whose deprecation warnings Twister (-Werror) would turn into errors.
zephyr_compile_options(-Wno-error)

target_sources(app PRIVATE src/main.c)
# The vectors of the RFC 9383 exchange (V9), the same as in the host test of lib/sensei_secure.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/sensei_secure/tests/generated)
