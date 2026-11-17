/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file scd41.h
 * @brief SCD41 CO2 sensor (node "sensei,scd41"): single shot with a start and a fetch call, ASC settings.
 *
 * Standard sensor API: sensor_sample_fetch() powers the sensor up, runs one single shot (5 s) and powers it
 * down again, unless the caller holds a pm_device_runtime_get() reference. Channels: SENSOR_CHAN_CO2 (ppm),
 * SENSOR_CHAN_AMBIENT_TEMP (degC), SENSOR_CHAN_HUMIDITY (%RH).
 *
 * Split API for schedulers that sleep during the measurement: hold a pm_device_runtime_get() reference (the
 * rail and the bus switch come on and the sensor is configured), call sensei_scd41_start(), sleep for the
 * returned time, call sensei_scd41_fetch(). Then drop the reference with pm_device_runtime_put() (rail off)
 * or keep it and call sensei_scd41_sleep() (the sensor stays powered and sleeps, ASC keeps running).
 *
 * The calls of one device must not run at the same time; the Sensirion library and its bus are shared with the
 * SGP41 driver and serialised internally. Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_SCD41_H_
#define SENSEI_DRIVERS_SCD41_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check that an SCD41 answers (serial number and variant). Powers the sensor up and down once.
 * @return 0 if present, -ENODEV if not. The result is kept.
 */
int sensei_scd41_probe(const struct device *dev);

/**
 * @brief Automatic self calibration settings. The sensor keeps them in RAM, so the driver writes them after
 *        every cold start. Applied at once if the sensor is powered.
 *
 * ASC only converges if the sensor runs for hours without a power cut, and the sensor counts one single shot
 * as 5 min. With a gated rail turn it off; if the sensor stays powered, scale the periods to the single-shot
 * period (see sensei_scd41_asc_hours() in sensei_sensor_math.h).
 *
 * @param initial_hours ASC initial period (sensor default 44), a multiple of 4
 * @param standard_hours ASC standard period (sensor default 156), a multiple of 4
 */
int sensei_scd41_set_asc(const struct device *dev, bool enable, uint16_t initial_hours, uint16_t standard_hours);

/** @brief Ambient pressure in Pa (700 to 1200 hPa) for the next single shots. 0 turns the compensation off. */
void sensei_scd41_set_pressure(const struct device *dev, uint32_t pressure_pa);

/**
 * @brief Trigger one single shot (wakes the sensor first if it sleeps). Needs a pm_device_runtime_get()
 *        reference.
 * @return ms until sensei_scd41_fetch() has the result (5000), or a negative errno
 */
int sensei_scd41_start(const struct device *dev);

/**
 * @brief Read the result of the single shot. The values are then available through sensor_channel_get().
 * @return 0, -EBUSY if the result is not ready yet (call again later), or a negative errno
 */
int sensei_scd41_fetch(const struct device *dev);

/** @brief Send the power-down command (about 1 uA). The next sensei_scd41_start() wakes the sensor (30 ms). */
int sensei_scd41_sleep(const struct device *dev);

/**
 * @brief Forced recalibration against a known CO2 concentration (commissioning in fresh air: 420 ppm).
 *        The sensor must have measured for at least 3 min before; needs a pm_device_runtime_get() reference.
 * @param correction out, the correction in ppm (offset by 0x8000 removed); NULL to ignore
 * @return 0 or a negative errno (-EIO if the sensor refused)
 */
int sensei_scd41_forced_recalibration(const struct device *dev, uint16_t target_ppm, int16_t *correction);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_SCD41_H_ */
