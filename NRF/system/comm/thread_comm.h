/*
 * ----------------------------------------------------------------------
 *
 * File: thread_comm.h
 *
 * Last edited: 6.08.2024
 *
 * Copyright (C) 2024, ETH Zurich and University of Bologna.
 *
 * Authors:
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

#ifndef THREAD_COMM_H_
#define THREAD_COMM_H_

#include "zephyr/kernel.h"

/**
 * @brief Initialise the communication thread
 * @note comm mutex must be held when this function is called.
 * @return negative on error, 0 otherwise.
 */
int thread_comm_init();

/**
 * @brief Start the comm thread.
 * @note May only be called once.
 */
void thread_comm_start();

/** @brief Comm thread ID */
extern k_tid_t comm_t;

#endif /* THREAD_COMM_H_ */
