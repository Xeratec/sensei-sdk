# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

target_sources_ifdef(CONFIG_SENSEI_PWR app PRIVATE
    pwr.c
    thread_pwr.c
)

target_sources_ifdef(CONFIG_SENSEI_PWR_CHARGE app PRIVATE pwr_charge.c)
target_sources_ifdef(CONFIG_SENSEI_PWR_CHECK app PRIVATE pwr_check.c)
target_sources_ifdef(CONFIG_SENSEI_PWR_RAM_POWERDOWN app PRIVATE pwr_ram.c)

