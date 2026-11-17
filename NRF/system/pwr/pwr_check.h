/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/** @file pwr_check.h
 *  @brief Find the devices that keep the system from its sleep floor (CONFIG_SENSEI_PWR_CHECK).
 */

#ifndef SENSEI_PWR_CHECK_H_
#define SENSEI_PWR_CHECK_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>

/** A device that is not suspended. */
struct sensei_pwr_dev_info {
  const struct device *dev;
  enum pm_device_state state; /**< PM_DEVICE_STATE_ACTIVE or SUSPENDING (or SUSPENDED with a held reference) */
  uint32_t usage;             /**< runtime PM references held (0 when runtime PM is off for the device) */
  bool runtime;               /**< runtime PM is enabled: the device suspends by itself once nobody holds it */
};

/** Called once per device that is not suspended. */
typedef void (*sensei_pwr_idle_cb)(const struct sensei_pwr_dev_info *info, void *user);

/**
 * @brief Walk all devices that have device PM and report those that are not suspended.
 *
 * A device is reported when its state is not SUSPENDED or OFF, or when runtime PM holds a reference on it.
 * Devices without device PM are not looked at. Call it from a thread, when the system is meant to be idle: a
 * device that is being used right now is reported too.
 *
 * @param cb   called for each device found (may be NULL to only count)
 * @param user passed to cb
 * @return number of devices reported
 */
int sensei_pwr_check_idle(sensei_pwr_idle_cb cb, void *user);

#endif /* SENSEI_PWR_CHECK_H_ */
