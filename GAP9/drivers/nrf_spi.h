/*
 * ----------------------------------------------------------------------
 *
 * File: nrf_spi.h
 *
 * Last edited: 22.08.2024
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

#ifndef __NRF_SPI_H__
#define __NRF_SPI_H__

#include "pmsis.h"
#include "bsp/bsp.h"

#ifdef CONFIG_DRIVER_NRFSPI_LOG

#define NRFSPI_LOG_TAG "NRFSPI"

#define NRFSPI_LOG_TRC(fmt, ...) PI_LOG_TRC(NRFSPI_LOG_TAG, fmt, ##__VA_ARGS__)
#define NRFSPI_LOG_DBG(fmt, ...) PI_LOG_DBG(NRFSPI_LOG_TAG, fmt, ##__VA_ARGS__)
#define NRFSPI_LOG_ERR(fmt, ...) PI_LOG_ERR(NRFSPI_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define NRFSPI_LOG_TRC(fmt, ...)
#define NRFSPI_LOG_DBG(fmt, ...)
#define NRFSPI_LOG_ERR(fmt, ...)
#endif

ssize_t pi_nrfspi_read(struct pi_device *device, uint32_t ext_addr,
                                       void *buffer, uint32_t size, pi_evt_t *evt);

/// @brief NRF SPI configuration structure
typedef struct pi_nrfspi_conf
{
    int gpio_irq;
    int32_t (*bsp_open)(pi_device_t*);  //!< Open function to call to initialize the BSP part of the module
    int32_t (*bsp_close)(pi_device_t*); //!< Close function to call to deinitialize the BSP part of the module
    void*   bsp_conf;                   //!< BSP related data (May vary depending on the board used)
}pi_nrfspi_conf_t;

/// @brief NRF SPI data structure
typedef struct
{
    uint8_t reentrance_cnt; //!< Open counter
} pi_nrfspi_data_t;

/// @brief NRF SPI API instance
extern pi_device_api_t nrfspi_api;

#endif