/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file bh1730.h
 * @brief BH1730FVC light sensor (node "sensei,bh1730"): one-time measurement of both channels.
 *
 * Standard sensor API: sensor_sample_fetch() powers the sensor up (if the caller holds no
 * pm_device_runtime_get() reference), runs one measurement (about 110 ms) and powers it down. Channels:
 * SENSOR_CHAN_LIGHT (lux), SENSEI_BH1730_CHAN_VIS_RAW and SENSEI_BH1730_CHAN_IR_RAW (counts, val1).
 *
 * Split API: hold a pm_device_runtime_get() reference (POWER bit set), call sensei_bh1730_start(), sleep, call
 * sensei_bh1730_fetch(). Dropping the reference clears the POWER bit (the shield has no rail switch for this
 * sensor). Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_BH1730_H_
#define SENSEI_DRIVERS_BH1730_H_

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Raw counts of the visible+IR channel (DATA0) and of the infrared channel (DATA1). */
#define SENSEI_BH1730_CHAN_VIS_RAW ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 0))
#define SENSEI_BH1730_CHAN_IR_RAW ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

/**
 * @brief Check that a BH1730 answers (part ID) and put it into power-down. The result is kept.
 * @return 0 if present, -ENODEV if not
 */
int sensei_bh1730_probe(const struct device *dev);

/**
 * @brief Start one measurement. Needs a pm_device_runtime_get() reference.
 * @return ms until sensei_bh1730_fetch() has the result, or a negative errno
 */
int sensei_bh1730_start(const struct device *dev);

/**
 * @brief Read the result. The values are then available through sensor_channel_get().
 * @return 0, -EBUSY if the result is not ready yet (call again later), or a negative errno
 */
int sensei_bh1730_fetch(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_BH1730_H_ */
