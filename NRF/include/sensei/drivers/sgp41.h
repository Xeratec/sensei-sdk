/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sgp41.h
 * @brief SGP41 VOC and NOx sensor (node "sensirion,sgp41"): raw ticks, conditioning, heater off.
 *
 * The driver delivers raw ticks. The VOC and NOx index come from lib/sensei_gas_index (it keeps state across
 * samples, so the application owns it). Hold a pm_device_runtime_get() reference while the sensor is in use:
 * it switches the rail and the bus switch. After a cold start the sensor needs conditioning (10 commands, 1 s
 * apart) before the NOx raw value is valid; sensor_sample_fetch() runs the whole cycle for simple uses.
 *
 * Compensation ticks: rh = %RH * 65535 / 100, t = (degC + 45) * 65535 / 175 (sensei_sgp41_rh_ticks() and
 * sensei_sgp41_t_ticks() in sensei_sensor_math.h). The defaults are 0x8000 (50 %RH) and 0x6666 (25 degC).
 *
 * Standard sensor API: sensor_sample_fetch() powers the sensor up (if the caller holds no reference), runs
 * CONFIG_SGP41_SENSEI_CONDITIONING_S s of conditioning, one measurement, and powers it down. Channels:
 * SENSEI_SGP41_CHAN_VOC_RAW and SENSEI_SGP41_CHAN_NOX_RAW (val1 = ticks). Untested on hardware.
 */

#ifndef SENSEI_DRIVERS_SGP41_H_
#define SENSEI_DRIVERS_SGP41_H_

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Raw VOC ticks (SRAW_VOC) and raw NOx ticks (SRAW_NOX) of the last measurement. */
#define SENSEI_SGP41_CHAN_VOC_RAW ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 0))
#define SENSEI_SGP41_CHAN_NOX_RAW ((enum sensor_channel)(SENSOR_CHAN_PRIV_START + 1))

/** Conditioning commands the sensor needs after a cold start, 1 s apart. */
#define SENSEI_SGP41_CONDITIONING_STEPS 10

/**
 * @brief Check that an SGP41 answers (serial number and self test). Powers the sensor up and down once.
 * @return 0 if present, -ENODEV if not. The result is kept.
 */
int sensei_sgp41_probe(const struct device *dev);

/**
 * @brief One conditioning command (takes 50 ms). Needs a pm_device_runtime_get() reference.
 * @param sraw_voc out, raw VOC ticks (valid even in conditioning)
 */
int sensei_sgp41_condition(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks, uint16_t *sraw_voc);

/**
 * @brief One measurement (takes 50 ms). The sensor keeps its heater on until sensei_sgp41_heater_off() or
 *        the power-down. The result is also available through sensor_channel_get().
 */
int sensei_sgp41_measure(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks, uint16_t *sraw_voc,
                         uint16_t *sraw_nox);

/** @brief Heater off (idle mode, about 2 uA). Use it when the rail stays on between cycles. */
int sensei_sgp41_heater_off(const struct device *dev);

/** @brief Compensation ticks for sensor_sample_fetch(). */
void sensei_sgp41_set_compensation(const struct device *dev, uint16_t rh_ticks, uint16_t t_ticks);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_DRIVERS_SGP41_H_ */
