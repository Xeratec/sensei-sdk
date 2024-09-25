/*
 * ----------------------------------------------------------------------
 *
 * File: transport_internal.h
 *
 * Last edited: 13.09.2024
 *
 * Copyright (C) 2024, ETH Zurich and University of Bologna.
 *
 * Authors:
 * - Philipp Schilk (schilkp@ethz.ch), ETH Zurich
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


#ifndef TRANSPORT_INTERNAL_H_
#define TRANSPORT_INTERNAL_H_

#include "comm/transport/transport.h"

#include <zephyr/kernel.h>

// ======== Interface for backends =================================================================

/**
 * \defgroup BACKEND_EVT Events raised by the various backends / backend components for the transport layer manager.
 * @{
 */
#define BACKEND_EVT_BACKEND_STATUS_CHANGED (0x1U << 0U) //!< Backend Event: Backend status changed.
#define BACKEND_EVT_PACKET_DROPPED         (0x1U << 1U) //!< Backend Event: A Packet was dropped
/** @} */

extern struct k_event backend_events;

#define BACKEND_RAW_TX_DATA_SIZE (1024)

struct raw_tx_data {
  size_t size;
  uint8_t data[BACKEND_RAW_TX_DATA_SIZE];
};

#define BACKEND_RAW_RX_DATA_SIZE (1024)

/**
 * @brief A chunk of raw, received data.
 */
struct raw_rx_data {
  size_t size;                            //!< Size, in bytes
  enum backend origin;                    //!< backend that received this chunk
  uint8_t data[BACKEND_RAW_RX_DATA_SIZE]; //!< actual data.
};

/** @brief RX Input queue */
extern struct k_msgq raw_rx_q;

int transport_submit_to_active_backend(enum endpoint e, struct raw_tx_data *d);

#endif /* TRANSPORT_INTERNAL_H_ */
