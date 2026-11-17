/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lis2duxs12_extra.h
 * @brief What the Zephyr LIS2DUX12 driver does not offer: the wake-up (motion) interrupt on INT1 and
 *        the deep power-down state.
 *
 * Use it next to the Zephyr sensor node. Start the accelerometer with the sensor API first (set a
 * sampling frequency, the ULP modes use the least power), then call sensei_lis2duxs12_wake_enable().
 * INT1 is P0.23 on the SENSEI base board (int1-gpios of the node).
 * The functions talk to the sensor on their own, so do not call them while a sensor_sample_fetch()
 * runs in another thread. Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_LIS2DUXS12_EXTRA_H_
#define SENSEI_DRIVERS_LIS2DUXS12_EXTRA_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sensei_lis2duxs12_wake_cfg {
  uint16_t threshold_mg; /**< motion threshold; the sensor takes steps of FS/256 up to 63 of them, then FS/64, so the
                              largest is about 0.98 * FS (see sensei_lis2duxs12_wake_ths()) */
  uint8_t fs_g;          /**< full scale set in the Zephyr driver: 2, 4, 8 or 16 */
  uint8_t duration;      /**< samples above the threshold, 0 to 3 */
};

/**
 * @brief Enable wake-up detection and route it to INT1 (high level while the event is latched).
 * With inactivity the sensor drops to 1.6 Hz and comes back to the set rate on motion.
 */
int sensei_lis2duxs12_wake_enable(const struct i2c_dt_spec *i2c, const struct sensei_lis2duxs12_wake_cfg *cfg);

/** @brief Disable wake-up detection and the INT1 routing. */
int sensei_lis2duxs12_wake_disable(const struct i2c_dt_spec *i2c);

/** @brief Read the wake-up flag. Reading clears the latched event and INT1. */
int sensei_lis2duxs12_wake_event(const struct i2c_dt_spec *i2c, bool *woke);

/** @brief Deep power-down (lowest power). Configuration is lost, set it up again after waking. */
int sensei_lis2duxs12_deep_power_down(const struct i2c_dt_spec *i2c);

/** @brief Leave deep power-down. Set the sampling frequency again through the sensor API. */
int sensei_lis2duxs12_wake_from_deep_power_down(const struct i2c_dt_spec *i2c);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_LIS2DUXS12_EXTRA_H_ */
