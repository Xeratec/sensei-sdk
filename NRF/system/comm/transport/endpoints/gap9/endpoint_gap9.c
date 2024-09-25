/*
 * ----------------------------------------------------------------------
 *
 * File: endpoint_gap9.c
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

#include "comm/thread_comm.h"

#include "comm/transport/transport.h"
#include "comm/transport/transport_internal.h"
#include "comm/transport/endpoints/gap9/endpoint_gap9.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

// ======== Defines/Variables ======================================================================
LOG_MODULE_REGISTER(endpoint_gap9, CONFIG_SENSEI_COMM_LOG_LEVEL);

// Current backend state (bool):
atomic_t endpoint_gap9_available = ATOMIC_INIT(false);

// Input:
K_MSGQ_DEFINE(gap9_tx_q,                     // Name
              sizeof(struct comm_tx_packet), // Entry size
              32,                            // Length
              4);                            // Alignment

// Output:
K_MSGQ_DEFINE(gap9_rx_q,                     // Name
              sizeof(struct comm_rx_packet), // Entry size
              3,                             // Length
              4);                            // Alignment

// ======== Functions ==============================================================================

int endpoint_gap9_init() {
  int ret = 0;

  atomic_set(&endpoint_gap9_available, true);

  LOG_DBG("Initialized.");

  return ret;
}

int gap9_send(void *data, size_t len) {

  LOG_DBG("Sending %d bytes to GAP9.", len );
  // Attempt to allocate space and encode:
  struct raw_tx_data *tx_packet;

  tx_packet = k_malloc(sizeof(struct raw_tx_data));
  if (tx_packet == NULL) {
    LOG_ERR("Failed to allocate memory for response.");
    return -1;
  }

  tx_packet->size = len;
  memcpy(tx_packet->data, data, len);


  // Attempt to send:
  if (transport_submit_to_active_backend(ENDPOINT_GAP9, tx_packet) < 0) {
    LOG_WRN("Failed to submit response. Dropping.");
    k_free(tx_packet);
    return -1;
  }

  k_free(tx_packet);
  return 0;
}