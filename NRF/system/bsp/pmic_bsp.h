/*
 * ----------------------------------------------------------------------
 *
 * File: pmic_bsp.h
 *
 * Last edited: 30.10.2025
 *
 * Copyright (c) 2024 ETH Zurich and University of Bologna
 *
 * Authors:
 * - Philipp Schilk (schilkp@ethz.ch), ETH Zurich
 * - Philip Wiese (wiesep@iis.ee.ethz.ch), ETH Zurich
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PMIC_BSP_H_
#define PMIC_BSP_H_

#include "max77654.h"

/**
 * @brief Hardware-specific read-register function as required by the MAX77654 driver.
 * see max77654.h
 */
max77654_err_t pmic_bsp_read_regs(uint8_t reg_adr, uint32_t n, uint8_t *buf);

/**
 * @brief Hardware-specific write-register function as required by the MAX77654 driver.
 * see max77654.h
 */
max77654_err_t pmic_bsp_write_regs(uint8_t reg_adr, uint32_t n, const uint8_t *buf);

/**
 * @brief Hardware-specific log function as required by the MAX77654 driver.
 * see max77654.h
 */
void pmic_bsp_log(char *msg, bool is_err, bool has_int_arg, uint32_t arg);

/**
 * @brief Hardware-specific adc-read function as required by the MAX77654 driver.
 * see max77654.h
 */
max77654_err_t pmic_bsp_adc_read(struct max77654_adc_reading *reading);

/**
 * @brief Adc-read function without the 1 ms wait of pmic_bsp_adc_read(), for max77654_measure_dischg_ua(): the NULL
 * and the DISCHG conversion follow each other (a busy wait of 10 us after the channel switch, the channel is set up on
 * the first call). Use it as the adc_read_fast of the MAX77654 handle.
 */
max77654_err_t pmic_bsp_adc_read_fast(struct max77654_adc_reading *reading);

#ifdef CONFIG_SENSEI_PMIC_ADC_EXTERNAL
/**
 * @brief One conversion of the PMIC AMUX input, done by the application (CONFIG_SENSEI_PMIC_ADC_EXTERNAL).
 *
 * With this option pmic_bsp_adc_read() and pmic_bsp_adc_read_fast() call this function instead of the Zephyr ADC
 * API. The application must define it; without a definition the link fails. The application owns the SAADC.
 *
 * Contract: return one conversion of the AMUX input (SENSEI board: AIN3 = P0.07) whose sampling instant is at
 * least settle_us after the call. The caller has switched the AMUX before the call. pmic_bsp_adc_read() passes
 * 1000, pmic_bsp_adc_read_fast() passes PMIC_BSP_AMUX_SETTLE_US (10). Two fast reads follow each other (the NULL and
 * the DISCHG conversion of max77654_measure_dischg_ua()) and should be taken close in time: the AMUX draws current
 * while it is on. The function may block (thread context only, not from an interrupt).
 *
 * @param settle_us Minimum time between the call and the sampling instant (us).
 * @param reading   Out: raw value (not negative), adc_max_reading and adc_fullscale_mV. The voltage is
 *                  reading x adc_fullscale_mV / adc_max_reading, so adc_max_reading is the value at full scale
 *                  (2^resolution for a single ended SAADC input; the Zephyr path of this BSP reports 2^resolution - 1).
 * @retval E_MAX77654_SUCCESS, or an error (for example E_MAX77654_COM_ERR when the conversion failed or timed out).
 */
max77654_err_t pmic_bsp_adc_ext_read(uint32_t settle_us, struct max77654_adc_reading *reading);
#endif

/**
 * @brief Reset I2C port hotfix
 * I2CB switch causes a glitch which confuses the PMIC. This performs
 * a dummy read (which errors, but can be ignored) to reset the PMIC's I2C
 * interface.
 */
void pmic_i2c_reset_hotfix();

#endif /* PMIC_BSP_H_ */
