/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file bme688.h
 * @brief BME688 (node "bosch,bme688"): Bosch BME68x API in forced mode with one heater step.
 *
 * Standard sensor API: sensor_sample_fetch() runs one forced measurement and waits for it (about 190 ms).
 * Channels: SENSOR_CHAN_AMBIENT_TEMP (degC), SENSOR_CHAN_PRESS (kPa), SENSOR_CHAN_HUMIDITY (%RH),
 * SENSOR_CHAN_GAS_RES (ohm; 0 if the gas reading or the heater was not stable).
 *
 * Split API: sensei_bme688_start(), sleep, sensei_bme688_fetch(). The sensor sleeps between forced
 * measurements, so there is nothing to power-gate. Nothing touches the sensor before the first call, so the
 * driver can be set up before the power rails of the board are up. BSEC is not used. Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_BME688_H_
#define SENSEI_DRIVERS_BME688_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check that a BME688 answers and set it up. The result is kept.
 * @return 0 if present, -ENODEV if not
 */
int sensei_bme688_probe(const struct device *dev);

/**
 * @brief Change the heater step (the devicetree has the defaults). Applies from the next measurement.
 * @param temp_c 200 to 400
 * @param duration_ms 1 to 4032
 */
int sensei_bme688_set_heater(const struct device *dev, uint16_t temp_c, uint16_t duration_ms);

/**
 * @brief Switch the gas heater off or on (on by default). Applies from the next measurement.
 *
 * Off: no heater current, no self-heating of the temperature and humidity reading, and a shorter measurement
 * (no heater duration). SENSOR_CHAN_GAS_RES then reads 0. Can be called before the chip is set up.
 */
int sensei_bme688_set_heater_enabled(const struct device *dev, bool enabled);

/**
 * @brief Start one forced measurement.
 * @return ms until sensei_bme688_fetch() has the result, or a negative errno
 */
int sensei_bme688_start(const struct device *dev);

/**
 * @brief Read the result. The values are then available through sensor_channel_get().
 * @return 0, -EBUSY if the result is not ready yet (call again later), or a negative errno
 */
int sensei_bme688_fetch(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_BME688_H_ */
