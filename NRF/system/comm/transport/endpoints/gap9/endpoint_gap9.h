/*
 * ----------------------------------------------------------------------
 *
 * File: endpoint_gap9.h
 *
 * Last edited: 13.09.2024
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

#ifndef ENDPOINT_GAP9_H_
#define ENDPOINT_GAP9_H_

#include <zephyr/kernel.h>

/**
 * @brief Transport layer input queue.
 * @note Contains 'comm_tx_packet' structs.
 */
extern struct k_msgq gap9_tx_q;

/**
 * @brief GAP9 Transport layer output queue.
 * @note Contains 'comm_rx_packet' structs.
 */
extern struct k_msgq gap9_rx_q;

extern atomic_t endpoint_gap9_available;

int endpoint_gap9_init();

/**
 * @brief Send a message to the GAP9.
 * @param data Pointer to the data to send.
 * @param len Length of the data to send.
 */
int gap9_send(void *data, size_t len);

#endif /* ENDPOINT_GAP9_H_ */