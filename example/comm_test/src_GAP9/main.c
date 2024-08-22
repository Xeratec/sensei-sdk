/*
 * ----------------------------------------------------------------------
 *
 * File: main.c
 *
 * Last edited: 19.06.2024
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

/***Include****/
#include "pmsis.h"
#include <stdint.h>
#include <stdio.h>

#include "bsp/bsp.h"

#ifdef CONFIG_APP_LOG
#define APP_LOG_TAG "App"
#define APP_LOG_TRC(fmt, ...) PI_LOG_TRC(APP_LOG_TAG, fmt, ##__VA_ARGS__)
#define APP_LOG_DBG(fmt, ...) PI_LOG_DBG(APP_LOG_TAG, fmt, ##__VA_ARGS__)
#define APP_LOG_ERR(fmt, ...) PI_LOG_ERR(APP_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define APP_LOG_TRC(fmt, ...)
#define APP_LOG_DBG(fmt, ...)
#define APP_LOG_ERR(fmt, ...)
#endif

#define APP_DELAY_US 1000000 // 1s delay

pi_device_t *nrf_device;

int main(void) {
  pi_err_t err = PI_OK;

  printf("Entering main controller\n");

  err = pi_open(PI_SPI_NRF, &nrf_device);
  if (err) {
    APP_LOG_ERR("Failed to open NRF device\n");
    goto error_handler;
  }
  APP_LOG_TRC("NRF device opened\n");

  while (1) {
    pi_time_wait_us(APP_DELAY_US);
    pi_nrfspi_read(nrf_device, 0, NULL, 0, NULL);
  }
  return 0;

error_handler:
  printf("Test failed\n");
  return err;
}
