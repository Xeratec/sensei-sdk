/*
 * ----------------------------------------------------------------------
 *
 * File: pwr_priv.h
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

#ifndef PWR_PRIV_H_
#define PWR_PRIV_H_

/* Internal to NRF/system/pwr (pwr.c and thread_pwr.c). Applications use pwr/pwr.h. */

#include "zephyr/kernel.h"

#include "max77654.h"

#define PWR_MUTEX_TIMEOUT K_MSEC(50)

/** Held to operate on the PMIC. */
extern struct k_mutex pwr_mutex;

/** PMIC driver handle. */
extern struct max77654_h pmic_h;

/**
 * @brief Initialise the power thread
 * @note pwr mutex must be held when this function is called.
 * @return negative on error, 0 otherwise.
 */
int thread_pwr_init(void);

/**
 * @brief Start the pwr thread.
 * @note May only be called once.
 */
void thread_pwr_start(void);

/**
 * @brief Log the currents. @note pwr mutex must be held.
 * @return negative on error, 0 otherwise.
 */
int measure_currents(void);

/**
 * @brief Log the voltages. @note pwr mutex must be held.
 * @return negative on error, 0 otherwise.
 */
int measure_voltages(void);

/** @brief Current battery charge state, in percent. 0 before the first reading. */
extern atomic_t battery_perc;

/** @brief Current battery voltage, in millivolts. 0 before the first reading. */
extern atomic_t battery_mV;

/** @brief Current GAP9 power state (1 on, 0 off). */
extern atomic_t gap9_pwr_state;

/** @brief Power thread ID */
extern k_tid_t pwr_t;

#endif /* PWR_PRIV_H_ */
