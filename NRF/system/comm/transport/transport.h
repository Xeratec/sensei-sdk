/*
 * ----------------------------------------------------------------------
 *
 * File: transport.h
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

#ifndef TRANSPORT_H_
#define TRANSPORT_H_

#include <zephyr/kernel.h>

// ======== Interface ==============================================================================

// Maximum length of a received message after deframing.
#define MAX_RX_LENGTH (1024)

enum packet_type {
  PACKET_TYPE_PC2NRF = 0x0,       //! Command to this device
  PACKET_TYPE_NRF2PC = 0x1,       //! Response from this device
  PACKET_TYPE_NRF2GAP9 = 0x2,     //! Command to GAP9
  PACKET_TYPE_GAP92BRF = 0x3,     //! Command from GAP9
  PACKET_TYPE_STREAM_CHUNK = 0x4, //! A single data chunk, during streaming.
  COUNT_PACKET_TYPE,
};

// All available backends, highest priority at first
enum backend {
  BACKEND_NONE = 0,
  BACKEND_SPI_GAP9,
  BACKEND_COUNT,
};

enum endpoint {
  ENDPOINT_GAP9,
  ENDPOINT_PC,
  ENDPOINT_COUNT,
};

/**
 * @brief A single packet that should be transmitted.
 */
struct comm_tx_packet {
  uint8_t *data; //!< Pointer to memory region.
  size_t size;   //!< Size of memory region request.
  enum packet_type type;
};

/**
 * @brief A single packet that was received.
 */
struct comm_rx_packet {
  uint8_t data[MAX_RX_LENGTH];
  size_t size;
  enum packet_type type;
};

/**
 * @brief Initialize all transport layer components.
 * @return negative on error, 0 otherwise.
 */
int transport_init();

/**
 * @brief Start all transport layer components.
 * @note May only be called once.
 */
void transport_start();

#endif /* TRANSPORT_H_ */
