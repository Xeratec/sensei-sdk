# Copyright (c) 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

# Suppress "unique_unit_address_if_enabled" to handle the overlapping peripheral addresses
# (flash-controller and kmu, power and clock).
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
