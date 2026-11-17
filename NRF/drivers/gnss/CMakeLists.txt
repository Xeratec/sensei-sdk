# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_GNSS_M10S gnss_m10s_i2c.c ubx.c)
