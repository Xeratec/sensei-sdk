/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ilps28qsw.h
 * @brief ILPS28QSW pressure sensor (node "st,ilps28qsw"): start and fetch as two steps.
 *
 * The standard sensor API (sensor_sample_fetch(), channels SENSOR_CHAN_PRESS in kPa and
 * SENSOR_CHAN_AMBIENT_TEMP) triggers one conversion and waits for it. A scheduler that sleeps in between
 * calls sensei_ilps28qsw_start(), sleeps for the returned time and calls sensei_ilps28qsw_fetch(). The sensor
 * powers itself down after each conversion, so there is nothing to gate. Nothing touches the sensor before
 * the first call. Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_ILPS28QSW_H_
#define SENSEI_DRIVERS_ILPS28QSW_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check that an ILPS28QSW answers and set it up. The result is kept.
 * @return 0 if present, -ENODEV if not
 */
int sensei_ilps28qsw_probe(const struct device *dev);

/**
 * @brief Trigger one conversion.
 * @return ms until sensei_ilps28qsw_fetch() has the result, or a negative errno
 */
int sensei_ilps28qsw_start(const struct device *dev);

/**
 * @brief Read the result. The values are then available through sensor_channel_get().
 * @return 0, -EBUSY if the result is not ready yet (call again later), or a negative errno
 */
int sensei_ilps28qsw_fetch(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_ILPS28QSW_H_ */
