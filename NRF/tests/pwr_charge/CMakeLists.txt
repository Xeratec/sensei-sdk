# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# Bindings of the PMIC stand-in (pwr) and of the retained RAM test driver (diag). The i2ca alias and the ADC channel of
# the board: sensei_sim.overlay; the PMIC and the GPIO lines: the overlay of the pwr test; the retention area of
# the counter: app.overlay of this test.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../pwr ${CMAKE_CURRENT_LIST_DIR}/../diag)
list(INSERT EXTRA_DTC_OVERLAY_FILE 0 ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay
     ${CMAKE_CURRENT_LIST_DIR}/../pwr/app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_pwr_charge_test)

# The PMIC emulator, the pwr_bsp.h of the test (the board hooks are fakes) and the retained RAM driver are shared with
# the pwr and diag tests.
target_sources(app PRIVATE src/main.c ../pwr/src/emul_max77654.c ../diag/src/test_retained_mem.c)
target_include_directories(app PRIVATE ../pwr/src)
