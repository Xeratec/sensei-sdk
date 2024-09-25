/*
 * ----------------------------------------------------------------------
 *
 * File: backend_spi_gap9.h
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

#ifndef BACKEND_SPI_GAP9_H_
#define BACKEND_SPI_GAP9_H_

#include <zephyr/kernel.h>

extern struct k_msgq spi_gap9_raw_tx_q;

/**
 * @brief Initialise the USB backend.
 * @return negative on error, 0 otherwise.
 */
int backend_spi_gap9_init();

/**
 * @brief Kickstart SPI transmission.
 */
void backend_spi_gap9_tx() ;

extern atomic_t backend_spi_gap9_available;

#endif /* BACKEND_SPI_GAP9_H_ */


