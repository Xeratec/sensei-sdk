/*
 * ----------------------------------------------------------------------
 *
 * File: thread_comm.c
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

#include "comm/thread_comm.h"
#include "comm/comm_common.h"

#include "config/tasks.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// ======== Definitions/Variables ==================================================================

LOG_MODULE_REGISTER(thread_comm, CONFIG_SENSEI_COMM_LOG_LEVEL);

/* The devicetree node identifier */
#define GPIO_NODE_gap9_irq DT_NODELABEL(gpio_gap9_irq)

static const struct gpio_dt_spec gpio_gap9_irq = GPIO_DT_SPEC_GET(GPIO_NODE_gap9_irq, gpios);

// Semaphore to indicate that an update should be performed:
K_SEM_DEFINE(gap9_update_sem, 0, 2);

// GAP9 interrupt callback:
static struct gpio_callback gap9_int_cb;
static void gap9_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins);

// Check the current GAP9 status:
static int check_status();

// Handle interrupts:
static int handle_interrupts();

// ======== Thread =================================================================================

int thread_comm_init() {
  // Setup GAP9 SPI int pin interrupt:
  if (gpio_pin_interrupt_configure_dt(&gpio_gap9_irq, GPIO_INT_EDGE_FALLING) < 0) {
    LOG_ERR("GPIO Int. config error!");
    return -1;
  }

  // Create a callback:
  gpio_init_callback(&gap9_int_cb, gap9_int_cb_handler, BIT(gpio_gap9_irq.pin));

  // Attach callback to pin interrupt:
  if (gpio_add_callback(gpio_gap9_irq.port, &gap9_int_cb) < 0) {
    LOG_ERR("GPIO callback add error!");
    return -1;
  }

  return 0;
}

static void thread_comm(void *_a, void *_b, void *_c) {
  LOG_INF("Running.");

  while (1) {
    int err = 0;

    // Grab comm mutex:
    if (k_mutex_lock(&comm_mutex, COMM_MUTEX_TIMEOUT) < 0) {
      LOG_ERR("Comm deadlock!");
      err = -1;
      goto skip_update;
    }

    // Grab current PMIC status, issuing charger on / charger off events:
    if (check_status() < 0) {
      err = -1;
    }

    // Handle interrupts:
    if (handle_interrupts() < 0) {
      err = -1;
    }

  skip_update:
    if (err < 0) {
      LOG_ERR("Error in communication thread!");
    }

    k_mutex_unlock(&comm_mutex);

    // Update on GAP9 interrupt but at least every PWR_THREAD_UPDATE_PERIOD_MS
    k_sem_take(&gap9_update_sem, K_MSEC(THREAD_COMM_UPDATE_PERIOD_MS));
  }
}

// ======== Functions ==============================================================================

static void gap9_int_cb_handler(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
  LOG_INF("GAP9 interrupt!");
  k_sem_give(&gap9_update_sem);
}

static int check_status() {
  LOG_DBG("Checking status...");
  return 0;
}

static int handle_interrupts() {
  LOG_DBG("Handling interrupts...");
  return 0;
}

// ======== Thread Setup ===========================================================================

static K_THREAD_STACK_DEFINE(thread_comm_stack, 1024);
static struct k_thread thread_comm_data;
k_tid_t comm_t;

void thread_comm_start() {

  comm_t = k_thread_create(&thread_comm_data,                                           // Thread data
                           thread_comm_stack, K_THREAD_STACK_SIZEOF(thread_comm_stack), // Thread Stack
                           thread_comm,                                                 // Entry
                           NULL, NULL, NULL,                                            // Args...
                           CONFIG_COMM_THREAD_PRIORITY,                                 // Priority
                           0,                                                           // Options
                           K_NO_WAIT                                                    // Delay
  );
  k_thread_name_set(comm_t, "comm");
}
