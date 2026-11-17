/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_sensor_math.h
 * @brief Unit conversions and timings of the sensor shield drivers (NRF/drivers/sensor). Plain C, no Zephyr.
 */

#ifndef SENSEI_SENSOR_MATH_H
#define SENSEI_SENSOR_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SGP41 humidity compensation ticks for a relative humidity in %RH (clamped to 0 to 100). */
uint16_t sensei_sgp41_rh_ticks(float rh_pct);

/** @brief SGP41 temperature compensation ticks for a temperature in degC (clamped to -45 to 130). */
uint16_t sensei_sgp41_t_ticks(float t_c);

/** @brief AS7331 die temperature in degC from the TEMP register. */
float sensei_as7331_temp_c(uint16_t raw);

/** @brief AS7331 channels. */
enum sensei_as7331_ch { SENSEI_AS7331_UVA, SENSEI_AS7331_UVB, SENSEI_AS7331_UVC };

/**
 * @brief AS7331 counts to irradiance in uW/cm2.
 *
 * Datasheet DS001047 v4-00, Figure 7: 385 / 347 / 794 counts per uW/cm2 for A / B / C at gain 2048x and
 * 64 ms. The 16-bit result registers are used (divider off).
 *
 * @param gain CREG1 GAIN value (gain = 2048 / 2^value)
 * @param time CREG1 TIME value (conversion time 2^time ms)
 */
float sensei_as7331_uw_cm2(enum sensei_as7331_ch ch, uint16_t counts, uint8_t gain, uint8_t time);

/**
 * @brief SCD41 ASC period in hours, scaled for single shots every period_s.
 *
 * The sensor counts one single shot as 5 min, so hours scale with 300 s / period. The sensor wants a
 * multiple of 4 h, at least 4.
 */
uint16_t sensei_scd41_asc_hours(uint16_t hours_at_5min, uint32_t period_s);

/** @brief BH1730 time for one measurement of both channels in ms, for the ITIME register value. */
uint32_t sensei_bh1730_meas_ms(uint8_t itime);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_SENSOR_MATH_H */
