# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

add_subdirectory_ifdef(CONFIG_ILPS28QSW_SENSEI ilps28qsw)
add_subdirectory_ifdef(CONFIG_LIS2DUXS12_EXTRA lis2duxs12_extra)
add_subdirectory_ifdef(CONFIG_SENSEI_SENSIRION_HAL sensirion_hal)
add_subdirectory_ifdef(CONFIG_SCD41_SENSEI scd41)
add_subdirectory_ifdef(CONFIG_SGP41_SENSEI sgp41)
add_subdirectory_ifdef(CONFIG_BME688_SENSEI bme688)
add_subdirectory_ifdef(CONFIG_AS7331_SENSEI as7331)
add_subdirectory_ifdef(CONFIG_BH1730_SENSEI bh1730)
