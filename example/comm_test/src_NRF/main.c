/*
 * ----------------------------------------------------------------------
 *
 * File: main.c
 *
 * Last edited: 5.07.2024
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

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/drivers/uart.h>

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>

// Power Subsystem
#include "pwr/pwr.h"
#include "pwr/pwr_common.h"
#include "pwr/thread_pwr.h"

// Communication Subsystem
#include "comm/comm.h"
#include "comm/comm_common.h"
#include "comm/thread_comm.h"

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 3000

LOG_MODULE_REGISTER(main);

static const struct device *const uart_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

int main(void) {

  LOG_INIT();

  LOG_INF("Communication Test on %s", CONFIG_BOARD);

  // Initialize and start power management
  pwr_init();
  pwr_start();

  // Initialize and start communication
  comm_init();
  comm_start();

  k_msleep(100);

  if (!device_is_ready(uart_dev)) {
    LOG_ERR("CDC ACM device not ready");
    return 0;
  }

  if (usb_enable(NULL)) {
    return 0;
  }
  LOG_INF("USB enabled");

  while (1) {
    k_msleep(SLEEP_TIME_MS);
    printf("Sleeping...\r\n");
  }
  return 0;
}
