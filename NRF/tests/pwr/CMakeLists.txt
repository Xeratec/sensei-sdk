# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# The PMIC binding is in dts/bindings. The i2ca alias and the ADC channel of the board: sensei_sim.overlay.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})
list(INSERT EXTRA_DTC_OVERLAY_FILE 0 ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_pwr_test)

target_sources(app PRIVATE src/main.c src/emul_max77654.c)
# bsp/pwr_bsp.h: the board hooks of the application, faked by the test
target_include_directories(app PRIVATE src)

# pwr_off_and_reboot() calls sys_reboot(): the test records it (it does not return, as the real one)
zephyr_ld_options(-Wl,--wrap=sys_reboot)
