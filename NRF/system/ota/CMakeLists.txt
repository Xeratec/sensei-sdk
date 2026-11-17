# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

target_sources(app PRIVATE
    sensei_ota.c
)

target_sources_ifdef(CONFIG_SENSEI_OTA_SMP_STREAM app PRIVATE sensei_smp_stream.c)
