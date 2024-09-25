/*
 * ----------------------------------------------------------------------
 *
 * File: transport.c
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


#include "comm/transport/backends/spi/backend_spi_gap9.h"
#include "comm/transport/endpoints/gap9/endpoint_gap9.h"
#include "comm/transport/transport.h"
#include "comm/transport/transport_internal.h"

#include "config/tasks.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>


// ======== Defines/Variables ======================================================================

LOG_MODULE_REGISTER(transport, CONFIG_SENSEI_COMM_LOG_LEVEL);

// Current backend mutex:
// Has to be held to acces the current backend state.
K_MUTEX_DEFINE(backend_mutex);


// Queue containing raw received data that is yet to be framed:
K_MSGQ_DEFINE(raw_rx_q,                   // Name
              sizeof(struct raw_rx_data), // Entry size
              10,                         // Length
              4);                         // Alignment

// Zephyr event to manage all events from the different backends.
K_EVENT_DEFINE(backend_events);

struct transport_backend {
  char name[32];
  atomic_t *backend_available;
  struct k_msgq *input_q;
  int (*fn_init)();
  void (*fn_start)();
  int (*fn_purge)();
  void (*fn_tx_kickstart)();
};

struct transport_endpoint {
  char name[32];
  atomic_t *endpoint_available;
  /**
   * @brief Transport layer input queue.
   * @note Contains 'comm_tx_packet' structs.
   */
  struct k_msgq *input_q;
  enum backend active_backend;
  enum backend compatible_backends[BACKEND_COUNT];
  int (*fn_init)();
};


atomic_t always_false = ATOMIC_INIT(0);

// All available backends, highest priority at lowest index:
static struct transport_backend backends[] = {
    [BACKEND_SPI_GAP9] =
        {
            .name = "SPI GAP9",
            .backend_available = &backend_spi_gap9_available,
            .input_q = &spi_gap9_raw_tx_q,
            .fn_init = backend_spi_gap9_init,
            .fn_start = 0,
            .fn_purge = 0,
            .fn_tx_kickstart = backend_spi_gap9_tx,

        },
    [BACKEND_NONE] =
        {
            .name = "NONE",
            .backend_available = &always_false,
            .input_q = 0,
            .fn_init = 0,
            .fn_start = 0,
            .fn_purge = 0,
            .fn_tx_kickstart = 0,
        },
};

static struct transport_endpoint endpoints[] = {
    [ENDPOINT_GAP9] =
      {
          .name = "GAP9",
          .endpoint_available = &endpoint_gap9_available,
          .input_q = &gap9_rx_q,
          .active_backend = BACKEND_SPI_GAP9,
          .compatible_backends = {BACKEND_SPI_GAP9},
          .fn_init = endpoint_gap9_init,
      },
    [ENDPOINT_PC] =
      {
          .name = "PC",
          .endpoint_available = &always_false,
          .input_q = 0,
          .active_backend = BACKEND_NONE,
          .compatible_backends = {},
          .fn_init = 0,
      },
};



BUILD_ASSERT((sizeof(backends) / sizeof(struct transport_backend)) == BACKEND_COUNT);
BUILD_ASSERT((sizeof(endpoints) / sizeof(struct transport_endpoint)) == ENDPOINT_COUNT);

// ======== Transport Manager Thread ===============================================================

static int thread_transport_mgr_init() { return 0; }

static void thread_transport_mgr(void *_a, void *_b, void *_c) {
  LOG_INF("Running.");
  while (1) {
    uint32_t evt = k_event_wait(&backend_events, 0xFFFFFFFF, false, K_FOREVER);

    if (k_mutex_lock(&backend_mutex, K_MSEC(10)) < 0) {
      LOG_ERR("Backend deadlock");
      continue;
    }

    k_event_clear(&backend_events, evt);

    if (evt & BACKEND_EVT_BACKEND_STATUS_CHANGED) {
      // Determine new active backend:
      enum backend new_backend = BACKEND_NONE;

      for (size_t i = 0; i < ENDPOINT_COUNT; i++) {
        if (atomic_get(endpoints[i].endpoint_available)) {
          for (size_t j = 0; j < BACKEND_COUNT; j++) {
            if (atomic_get(backends[endpoints[i].compatible_backends[j]].backend_available)) {
              new_backend = j;
              break;
            }
          }
          if (new_backend != endpoints[i].active_backend) {
            // Endpoint changed backend. TODO FLUSH (needed?)
            LOG_INF("Endpoint %s backend change: %s -> %s", endpoints[i].name, backends[endpoints[i].active_backend].name, backends[new_backend].name);
            endpoints[i].active_backend = new_backend;
          }
        }
      }
    }

    if (evt & BACKEND_EVT_PACKET_DROPPED) {
      LOG_WRN("Packet dropped");
      // TODO: Propagate to communication FSM (needed?)
    }

    k_mutex_unlock(&backend_mutex);
  }
}

static K_THREAD_STACK_DEFINE(thread_transport_mgr_stack, 1024);
static struct k_thread thread_transport_mgr_data;
k_tid_t transport_mgr_t;

static void thread_transport_mgr_start() {
  transport_mgr_t =
      k_thread_create(&thread_transport_mgr_data,                                                    // Thread data
                      thread_transport_mgr_stack, K_THREAD_STACK_SIZEOF(thread_transport_mgr_stack), // Thread Stack
                      thread_transport_mgr,                                                          // Entry
                      NULL, NULL, NULL,                                                              // Args...
                      CONFIG_COMM_TRANSPORT_THREAD_PRIORITY,                                         // Priority
                      0,                                                                             // Options
                      K_NO_WAIT                                                                      // Delay
      );
  k_thread_name_set(transport_mgr_t, "transport_mgr");
}

// ======== Operation ==============================================================================

int transport_get_current_backend(enum endpoint e, enum backend *b) {
  if (k_mutex_lock(&backend_mutex, K_MSEC(10)) < 0) {
    LOG_ERR("Backend deadlock");
    return -1;
  }

  *b = endpoints[e].active_backend;

  k_mutex_unlock(&backend_mutex);
  return 0;
}

int transport_submit_to_active_backend(enum endpoint e, struct raw_tx_data *d) {

  enum backend b = 0;

  while (1) {
    if (transport_get_current_backend(e, &b) < 0) {
      return -1;
    }
    struct k_msgq *q = backends[b].input_q;

    if (q == 0) {
      return -1;
    }

    if (k_msgq_put(q, d, K_MSEC(100)) == 0) {
      if (backends[b].fn_tx_kickstart != 0) {
        backends[b].fn_tx_kickstart();
      }
      break;
    }
  }

  return 0;
}

int transport_init() {

  if (thread_transport_mgr_init() < 0) {
    return -1;
  }

  // if (threads_packet_framing_init() < 0) {
  //   return -1;
  // }

  for (size_t i = 0; i < BACKEND_COUNT; i++) {
    struct transport_backend *backend = &backends[i];
    if (backend->fn_init != 0) {
      if (backend->fn_init() < 0) {
        LOG_ERR("Backend %s init failed!", backends->name);
        return -1;
      }
    }
  }

  for (size_t i = 0; i < ENDPOINT_COUNT; i++) {
    struct transport_endpoint *endpoint = &endpoints[i];
    if (endpoint->fn_init != 0) {
      if (endpoint->fn_init() < 0) {
        LOG_ERR("Endpoint %s init failed!", endpoints->name);
        return -1;
      }
    }
  }

  return 0;
}

void transport_start() {
  thread_transport_mgr_start();

  // threads_packet_framing_start();

  for (size_t i = 0; i < BACKEND_COUNT; i++) {
    if (backends[i].fn_start != 0) {
      backends[i].fn_start();
    }
  }
}
