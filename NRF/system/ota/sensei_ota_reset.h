/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_OTA_RESET_H_
#define SENSEI_OTA_RESET_H_

/**
 * @file sensei_ota_reset.h
 * @brief Who may reset the node into the MCUboot recovery, as a pure function (no Zephyr, host-testable).
 *
 * The SMP reset command has an optional `boot_mode`. With the retention boot mode the application stores it and
 * MCUboot reads it at the next boot: boot_mode 1 starts the serial recovery over USB, where MCUboot stays until
 * someone uses it (no time-out) and the node is off the air. A peer on Bluetooth must not be able to do that. The
 * recovery is for a person with a cable, so the request is accepted only while USB power is present.
 */

#include <stdbool.h>
#include <stdint.h>

/** Normal reset: boot_mode 0 (BOOT_MODE_TYPE_NORMAL of Zephyr). */
#define SENSEI_OTA_BOOT_MODE_NORMAL 0

/**
 * A reset with `boot_mode` is allowed: a normal reset always (it reverts nothing and MCUboot starts the application
 * again), any other boot mode only while USB power is present (`usb_power`).
 */
static inline bool sensei_ota_reset_allowed(uint8_t boot_mode, bool usb_power) {
  return boot_mode == SENSEI_OTA_BOOT_MODE_NORMAL || usb_power;
}

#endif /* SENSEI_OTA_RESET_H_ */
