# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

# Public headers (<sensei/drivers/...>) and the drivers, which are enabled by their devicetree nodes.
zephyr_include_directories(include)
add_subdirectory(drivers)

add_subdirectory_ifdef(CONFIG_SENSEI system)

# Field diagnostics (system/diag): a module of its own, it needs no SENSEI board (tests on QEMU use it).
add_subdirectory_ifdef(CONFIG_SENSEI_DIAG system/diag)
target_include_directories_ifdef(CONFIG_SENSEI_DIAG app PRIVATE system)

# RAM power-on at the start of the boot (MCUboot): like the diagnostics, it needs no SENSEI board
target_sources_ifdef(CONFIG_SENSEI_PWR_RAM_ON_BOOT app PRIVATE system/pwr/pwr_ram_boot.c)

# Portable libraries of lib/ (Zephyr part)
add_subdirectory_ifdef(CONFIG_SENSEI_LINK ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_link/zephyr sensei_link)
add_subdirectory_ifdef(CONFIG_SENSEI_BUSARB ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_busarb/zephyr sensei_busarb)
add_subdirectory_ifdef(CONFIG_SENSEI_GAP9 ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_gap9/zephyr sensei_gap9)
add_subdirectory_ifdef(CONFIG_SENSEI_LPWAN_PROTO ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_lpwan/zephyr sensei_lpwan)
add_subdirectory_ifdef(CONFIG_SENSEI_DELTA ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_delta/zephyr sensei_delta)
add_subdirectory_ifdef(CONFIG_SENSEI_SECURE ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_secure/zephyr sensei_secure)
add_subdirectory_ifdef(CONFIG_SENSEI_GAS_INDEX ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_gas_index/zephyr sensei_gas_index)
add_subdirectory_ifdef(CONFIG_SENSEI_SENSOR_MATH ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_sensor_math/zephyr sensei_sensor_math)
add_subdirectory_ifdef(CONFIG_SENSEI_PWR_MATH ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_pwr_math/zephyr sensei_pwr_math)
add_subdirectory_ifdef(CONFIG_SENSEI_DIAG_CODEC ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_diag/zephyr sensei_diag)
add_subdirectory_ifdef(CONFIG_SENSEI_CHARGE ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_charge/zephyr sensei_charge)
add_subdirectory_ifdef(CONFIG_SENSEI_UPLINK ${CMAKE_CURRENT_LIST_DIR}/../lib/sensei_uplink/zephyr sensei_uplink)

target_include_directories_ifdef(CONFIG_SENSEI app PRIVATE
    system
)
