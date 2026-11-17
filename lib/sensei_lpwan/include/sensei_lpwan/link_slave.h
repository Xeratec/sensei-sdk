/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

/**
 * @file link_slave.h
 * @brief Slave side of the nRF5340 link: frames, response storage, event queue, BUSY and EVENT.
 *
 * No Zephyr dependency. The transport (SPIS driver, GPIOs, lock) is behind link_slave_ops. See
 * lpwan_link.h for the transfer scheme.
 *
 * Call order per transfer, all from one thread: the transport arms the buffers it gets from the
 * `arm` hook, drives BUSY low (`set_busy(false)`) and waits. When the transfer ends it drives BUSY
 * high at once (ISR) and the thread calls link_slave_done().
 */

#ifndef SENSEI_LINK_SLAVE_H_
#define SENSEI_LINK_SLAVE_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_lpwan/lpwan_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_SLAVE_EVENT_SLOTS 4u

struct link_slave;

struct link_slave_ops {
  /** Arm one transfer of LPWAN_XFER_LEN bytes with these buffers. Must not block. */
  int (*arm)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
  /** Drive BUSY: true = busy (not ready), false = ready for a transfer. */
  void (*set_busy)(void *ctx, bool busy);
  /** Drive EVENT: true while an event waits. */
  void (*set_event)(void *ctx, bool pending);
  /** Protect the event queue against other threads (may be NULL). */
  void (*lock)(void *ctx);
  void (*unlock)(void *ctx);
};

/**
 * A valid request arrived. Execute it and call link_slave_respond() before returning. It is not
 * called again for a repeated seq (the stored response is sent instead).
 */
typedef void (*link_slave_request_fn)(struct link_slave *s, const sensei_LpwanRequest *req, void *user);

struct link_slave_frame {
  uint16_t len; /* frame length with header, 0 = empty */
  uint8_t data[LPWAN_XFER_LEN];
};

struct link_slave {
  const struct link_slave_ops *ops;
  void *ctx;
  link_slave_request_fn on_request;
  void *user;

  uint8_t rx[LPWAN_XFER_LEN];
  uint8_t tx[LPWAN_XFER_LEN];

  /* What is armed: 0 idle, 1 response, 2 event. */
  uint8_t armed;
  uint8_t tog; /* toggle bit of the current message */
  struct link_slave_frame response;
  bool response_pending;
  uint32_t last_seq;
  bool have_last;
  struct link_slave_frame events[LINK_SLAVE_EVENT_SLOTS];
  uint8_t ev_head, ev_count;
  bool master_error; /* ask the master to repeat its last frame: set error flag in our frames */

  /* counters */
  uint32_t n_transfers, n_bad_frames, n_repeats, n_dropped_events, n_requests;
};

void link_slave_init(struct link_slave *s, const struct link_slave_ops *ops, void *ctx,
                     link_slave_request_fn on_request, void *user);

/** Arm the first transfer. */
int link_slave_start(struct link_slave *s);

/** A transfer ended (`err` != 0: the driver reported an error). Parses it and arms the next. */
int link_slave_done(struct link_slave *s, int err);

/** Answer the request being handled (call inside on_request). */
int link_slave_respond(struct link_slave *s, const sensei_LpwanResponse *rsp);

/** Queue an event from any thread. Returns -ENOMEM and counts it if the queue is full. */
int link_slave_push_event(struct link_slave *s, const sensei_LpwanEvent *evt);

/** Number of events waiting. */
unsigned link_slave_events_pending(const struct link_slave *s);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LINK_SLAVE_H_ */
