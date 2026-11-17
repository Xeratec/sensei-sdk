# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(gnss)
add_subdirectory(sensor)
add_subdirectory_ifdef(CONFIG_PSRAM_APS12804O memory)
