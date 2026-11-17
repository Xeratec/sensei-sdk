/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file as7331.h
 * @brief AS7331 UV sensor (node "ams,as7331"): command mode, results in uW/cm2.
 *
 * Standard sensor API: sensor_sample_fetch() powers the sensor up (if the caller holds no
 * pm_device_runtime_get() reference), runs one measurement (2048 ms with the default) and powers it down.
 * Channels: SENSEI_AS7331_CHAN_UVA, _UVB, _UVC (uW/cm2) and SENSOR_CHAN_DIE_TEMP (degC).
 *
 * Split API: hold a pm_device_runtime_get() reference (bus switch closed, PD bit cleared), call
 * sensei_as7331_start(), sleep, call sensei_as7331_fetch(). Dropping the reference opens the bus switch and
 * sets the PD bit. Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_AS7331_H_
#define SENSEI_DRIVERS_AS7331_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Irradiance in uW/cm2. */
#define SENSEI_AS7331_CHAN_UVA ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 0))
#define SENSEI_AS7331_CHAN_UVB ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))
#define SENSEI_AS7331_CHAN_UVC ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 2))

/**
 * @brief Check that an AS7331 answers (chip ID). Closes the bus switch and puts the sensor into power-down
 *        afterwards. The result is kept.
 * @return 0 if present, -ENODEV if not
 */
int sensei_as7331_probe(const struct device *dev);

/**
 * @brief Gain and integration time (the devicetree has the defaults). Applies from the next measurement.
 * @param gain CREG1 GAIN, 0 to 11 (gain = 2048 / 2^gain)
 * @param time CREG1 TIME, 0 to 14 (conversion time = 2^time ms)
 */
int sensei_as7331_configure(const struct device *dev, uint8_t gain, uint8_t time);

/**
 * @brief Start one measurement.
 * @return ms until sensei_as7331_fetch() has the result, or a negative errno
 */
int sensei_as7331_start(const struct device *dev);

/**
 * @brief Read the result. The values are then available through sensor_channel_get().
 * @return 0, -EBUSY if the result is not ready yet (call again later), or a negative errno
 */
int sensei_as7331_fetch(const struct device *dev);

/** @brief State of the READY pin (ready-gpios). Returns -ENOTSUP if the node has none. */
int sensei_as7331_ready(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_AS7331_H_ */
