# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

# CONFIG_SENSEI builds the system code, which needs the board devices i2ca and an ADC channel: emulators on native_sim.
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/sensei_sim.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_ota_test)

target_sources(app PRIVATE src/main.c)

# sensei_ota.c asks MCUboot for the image state and resets the node: the test replaces these three calls (FFF fakes
# in src/main.c). The module itself is built as it is in an application.
zephyr_ld_options(
  -Wl,--wrap=boot_is_img_confirmed
  -Wl,--wrap=boot_write_img_confirmed
  -Wl,--wrap=sys_reboot)
