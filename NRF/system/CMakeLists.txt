# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(pwr) # every source of it is behind its own option
add_subdirectory_ifdef(CONFIG_SENSEI_CRYPTO crypto)
add_subdirectory_ifdef(CONFIG_SENSEI_OTA ota)
add_subdirectory_ifdef(CONFIG_SENSEI_MARKER marker)
add_subdirectory_ifdef(CONFIG_SENSEI_WDT wdt)

# The board support of the power subsystem (it needs the devices i2ca and an ADC channel of the board)
add_subdirectory_ifdef(CONFIG_SENSEI_PWR bsp)
add_subdirectory(third_party)