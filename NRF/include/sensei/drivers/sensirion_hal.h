/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Zephyr I2C glue of the Sensirion libraries (NRF/system/third_party/sensirion). The libraries talk to one
 * global bus, so a mutex serialises every use: acquire, call the library, release.
 */

#ifndef SENSEI_DRIVERS_SENSIRION_HAL_H_
#define SENSEI_DRIVERS_SENSIRION_HAL_H_

#include <stdint.h>
#include <zephyr/device.h>

/** Take the HAL lock and select the I2C bus for the library calls that follow. */
void sensirion_hal_acquire(const struct device *bus);

/** Release the HAL lock. */
void sensirion_hal_release(void);

/**
 * Deferred sleeps, valid between acquire and release. Some Sensirion commands sleep for the whole measurement
 * (SCD41 single shot: 5 s). After defer_begin(), sleeps of at least min_us are not executed but summed up;
 * defer_end() returns the sum in microseconds, so the caller can wait on its own schedule.
 */
void sensirion_hal_defer_begin(uint32_t min_us);
uint32_t sensirion_hal_defer_end(void);

#endif /* SENSEI_DRIVERS_SENSIRION_HAL_H_ */
