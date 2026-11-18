# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensei_pwr_ram_test)

# CONFIG_SENSEI_PWR_RAM_POWERDOWN needs the nRF5340, so the test builds the source itself. The library call
# power_down_unused_ram() is a fake; _image_ram_end, the linker symbol of the end of the image RAM, is set from Kconfig.
target_sources(app PRIVATE src/main.c ${CMAKE_CURRENT_LIST_DIR}/../../system/pwr/pwr_ram.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../system)
zephyr_ld_options(-Wl,--defsym=_image_ram_end=${CONFIG_SENSEI_TEST_IMAGE_END})
