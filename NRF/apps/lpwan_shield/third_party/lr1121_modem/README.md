<!--
SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
SPDX-License-Identifier: CC-BY-ND-4.0

Authors:
- Philip Wiese <wiesep@iis.ee.ethz.ch>
-->

# Semtech LR1121 Modem-E host driver

Copy of the driver v1.1.0 (`lr1121_modem_driver_version.c`) as used in the LPWAN shield
commissioning project. Copyright Semtech Corporation 2024, Clear BSD License (see the headers and
`LICENSES/BSD-3-Clause-Clear.txt`).

Not copied: the Semtech HAL (`lr1121_modem_hal.c`, `lr1121_hal.c`), replaced by
`src/lr1121_zephyr_hal.c`; the helper with `printk` output; the throughput test; the
Modem-E firmware image `lr1121_modem_firmware_2_0_2.h` (610 kB, only needed to flash the LR1121).

`lr1121_bootloader.*` is kept for a later update over the air and is not compiled.
