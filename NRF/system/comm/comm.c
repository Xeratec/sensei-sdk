/*
 * ----------------------------------------------------------------------
 *
 * File: comm.c
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

#include "comm.h"

#include "bsp/comm_bsp.h"
#include "comm/comm_common.h"
#include "comm/thread_comm.h"

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// ======== Defines/Variables ======================================================================

LOG_MODULE_REGISTER(comm, CONFIG_SENSEI_COMM_LOG_LEVEL);

static const struct device *comm_spi = DEVICE_DT_GET(DT_ALIAS(spia));

// COMM mutex:
// Has to be held to operate on the communication subsys. For the public comm interface this is managed automatically.
K_MUTEX_DEFINE(comm_mutex);

atomic_t comm_is_initialised = ATOMIC_INIT(0);
atomic_t comm_is_running = ATOMIC_INIT(0);

// ======== Functions ==============================================================================

int comm_init() {

  if (!device_is_ready(comm_spi)) {
    LOG_ERR("SPI device %s is not ready\n", comm_spi->name);
    return -1;
  }

  int err = 0;

  if (k_mutex_lock(&comm_mutex, COMM_MUTEX_TIMEOUT) < 0) {
    LOG_ERR("COMM Deadlock!");
    return -1;
  }

  if (comm_bsp_init()) {
    LOG_ERR("COMM BSP Init failed!");
    err = -1;
    goto cleanup;
  }

  LOG_INF("Driver init OK.");

  // Init comm thread:
  if (thread_comm_init() < 0) {
    LOG_ERR("comm thread init failed.");
    err = -1;
    goto cleanup;
  }

  atomic_set(&comm_is_initialised, true);

  k_sleep(K_MSEC(100));

cleanup:
  k_mutex_unlock(&comm_mutex);
  return err;
}

int comm_start() {
  __ASSERT_NO_MSG(atomic_get(&comm_is_initialised) == true);
  __ASSERT_NO_MSG(atomic_get(&comm_is_running) == false);

  if (comm_bsp_start()) {
    LOG_ERR("COMM BSP Start failed!");
    return -1;
  }

  thread_comm_start();

  atomic_set(&comm_is_running, true);

  return 0;
}

void comm_kill() {
  bool did_lock = false;
  // FIXME: This whole thing is very hacky and a workaround until the PMIC's FSM is working.

  for (uint32_t i = 0; i < 3; i++) {
    if (k_mutex_lock(&comm_mutex, K_MSEC(10)) < 0) {
      LOG_ERR("COMM MUTEX Timeout!");
    } else {
      did_lock = true;
      break;
    }
  }

  if (!did_lock) {
    LOG_ERR("Ignoring comm mutex. Forcibly shutting down!");
  }

  if (did_lock) {
    k_mutex_unlock(&comm_mutex);
  }
}