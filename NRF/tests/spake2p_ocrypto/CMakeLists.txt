# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_spake2p_ocrypto_test)

# The ocrypto backend of lib/sensei_secure (one file, no Zephyr in it) and the real ocrypto library of nrf_oberon,
# the build for a Cortex-M33 without the DSP extension (the QEMU model of mps2/an521/cpu0 stops on the DSP
# instructions of the nRF5340 build, an undefined instruction). No nRF Security, no PSA.
set(SECURE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/sensei_secure)
set(OBERON ${ZEPHYR_NRFXLIB_MODULE_DIR}/crypto/nrf_oberon)
target_sources(app PRIVATE src/main.c ${SECURE}/zephyr/sensei_spake2p_ocrypto.c)
target_include_directories(app PRIVATE ${SECURE}/zephyr ${SECURE}/tests/generated ${OBERON}/include)
target_link_libraries(app PRIVATE ${OBERON}/lib/cortex-m33+nodsp/soft-float/liboberon_3.0.20.a)
