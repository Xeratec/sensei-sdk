# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>

target_sources(app PRIVATE sensei_diag_rt.c)
target_sources_ifdef(CONFIG_SENSEI_DIAG_COREDUMP app PRIVATE sensei_diag_dump.c)
