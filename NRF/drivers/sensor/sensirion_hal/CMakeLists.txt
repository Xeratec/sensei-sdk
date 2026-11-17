# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

# The Sensirion common code (SCD4x repository, version 1.2.0, also used by the SGP41 driver) and the Zephyr
# I2C glue. Built when the SCD41 or the SGP41 driver is on.
set(SENSIRION_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../system/third_party/sensirion)

zephyr_library_named(sensei_sensirion_hal)
zephyr_library_sources(
  sensirion_hal.c
  ${SENSIRION_DIR}/common/sensirion_common.c
  ${SENSIRION_DIR}/common/sensirion_i2c.c
)
zephyr_include_directories(${SENSIRION_DIR}/common)
