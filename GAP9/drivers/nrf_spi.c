/*
 * ----------------------------------------------------------------------
 *
 * File: nrf_spi.c
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

#include "nrf_spi.h"
#include "bsp/bsp.h"

/// @brief   Driver's open function.
///
/// @param device Pointer to the device instance.
/// @return pi_err_t Error management
///          @arg PI_OK The driver is correctly opened
///          @arg PI_FAIL Failed to open the driver.
int32_t pi_nrfspi_open(pi_device_t* device)
{
    pi_assert(device);
    pi_assert(device->config);
    pi_assert(device->data);
    pi_err_t err = PI_OK;

    pi_nrfspi_conf_t* nrfspi_conf = (pi_nrfspi_conf_t*)device->config;
    pi_nrfspi_data_t* nrfspi_data = (pi_nrfspi_data_t*)device->data;

    if(nrfspi_data->reentrance_cnt)
    {
        nrfspi_data->reentrance_cnt++;
    }
    else
    {
        // Execute BSP open function if it has been set in the device tree
        if(NULL != nrfspi_conf->bsp_open)
        {
            if(PI_OK != nrfspi_conf->bsp_open(device))
            {
                PI_LOG_ERR("nrfspi", "Failed to open bsp\n");
                err = PI_FAIL;
            }
        }
        if(PI_OK == err)
        {
            // Implement your driver open process here
        }
    }
    return err;
}

/// @brief   Driver's close function.
///
/// @param device Pointer to the device instance.
/// @return pi_err_t Error management
///          @arg PI_OK The driver is correctly closed
///          @arg PI_FAIL Failed to close the driver.
int32_t pi_nrfspi_close(pi_device_t* device)
{
    pi_assert(device);
    pi_assert(device->config);
    pi_assert(device->data);
    pi_err_t err = PI_OK;
    pi_nrfspi_data_t* nrfspi_data = (pi_nrfspi_data_t*)device->data;
    pi_nrfspi_conf_t* nrfspi_conf = (pi_nrfspi_conf_t*)device->config;

    // Pin is active low
    pi_gpio_pin_write(nrfspi_conf->gpio_irq, 1);

    if(nrfspi_data->reentrance_cnt > 0)
    {
        nrfspi_data->reentrance_cnt--;
        if(nrfspi_data->reentrance_cnt == 0)
        {
            // Implement your driver close process here

            // Execute BSP close function if it has been set in the device tree
            if(NULL != nrfspi_conf->bsp_close)
            {
                if(PI_OK != nrfspi_conf->bsp_close(device))
                {
                    PI_LOG_ERR("nrfspi", "Failed to close bsp\n");
                    err = PI_FAIL;
                }
            }
        }
    }
    return err;
}

ssize_t pi_nrfspi_read(struct pi_device *device, uint32_t ext_addr,
                                       void *buffer, uint32_t size, pi_evt_t *evt)
{
    pi_assert(device);
    pi_assert(device->config);
    pi_assert(device->data);
    pi_err_t err = PI_OK;
    pi_nrfspi_data_t* nrfspi_data = (pi_nrfspi_data_t*)device->data;
    pi_nrfspi_conf_t* nrfspi_conf = (pi_nrfspi_conf_t*)device->config;

    NRFSPI_LOG_TRC("Triggering pin %d\n", nrfspi_conf->gpio_irq);

    pi_gpio_pin_write(nrfspi_conf->gpio_irq, 0);
    pi_time_wait_us(10);
    pi_gpio_pin_write(nrfspi_conf->gpio_irq, 1);

    return 0;
}

/// @brief Driver API definition'
pi_device_api_t nrfspi_api =
{
    .open  = &pi_nrfspi_open,
    .close = &pi_nrfspi_close,
    .read  = &pi_nrfspi_read,
};
