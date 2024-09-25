/*
 * ----------------------------------------------------------------------
 *
 * File: backend_spi_gap9.c
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

#include "comm/transport/backends/spi/backend_spi_gap9.h"
#include "comm/transport/transport.h"
#include "comm/transport/transport_internal.h"

#include "config/tasks.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/spi.h>

// ======== Defines/Variables ======================================================================

LOG_MODULE_REGISTER(backend_spi_gap9, CONFIG_SENSEI_COMM_LOG_LEVEL);

// Input/Ouput queues:
K_MSGQ_DEFINE(spi_gap9_raw_tx_q,               // Name
              sizeof(struct raw_tx_data), // Entry size
              10,                         // Length
              4);                         // Alignment

// Current backend state (bool):
atomic_t backend_spi_gap9_available = ATOMIC_INIT(false);

// Device handles
#define SPIOP      SPI_WORD_SET(8) | SPI_TRANSFER_MSB
static struct spi_dt_spec spi_gap9 = SPI_DT_SPEC_GET(DT_NODELABEL(spi_gap9), SPIOP, 0);

static struct raw_tx_data current_tx_data = {0};

// ======== SPI Operation  =========================================================================

int backend_spi_gap9_init(struct device *dev) {
  int ret = 0;

  if (!spi_is_ready_dt(&spi_gap9)) {
    LOG_ERR("SPI Channel A not ready");
    return -1;
  }

  atomic_set(&backend_spi_gap9_available, true);

  LOG_DBG("Initialized.");

  return ret;
}

void backend_spi_gap9_tx() {
  if (k_msgq_get(&spi_gap9_raw_tx_q, &current_tx_data, K_NO_WAIT) == 0) {
    // Start the transmission:
    LOG_INF("Transmitting %d bytes.", current_tx_data.size);

    const struct spi_buf tx_buf = {.buf = current_tx_data.data, .len = current_tx_data.size};
    const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

    // Start the transmission:
    spi_write_dt(&spi_gap9, &tx);
  }
}
