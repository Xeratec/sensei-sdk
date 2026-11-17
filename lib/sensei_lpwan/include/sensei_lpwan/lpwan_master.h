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
 * @file lpwan_master.h
 * @brief Master side of the nRF9151 link: request/response with seq, retries and timeouts, and
 *        event fetching. No Zephyr dependency. See lpwan_link.h.
 */

#ifndef SENSEI_LPWAN_MASTER_H_
#define SENSEI_LPWAN_MASTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_lpwan/lpwan_link.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lpwan_master_ops {
  /** One full-duplex SPI transfer of `len` bytes. Returns 0 or a negative errno. */
  int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
  /** Wait until BUSY is low. Returns 0, or -ETIMEDOUT. */
  int (*wait_ready)(void *ctx, uint32_t timeout_ms);
  /** True while the slave signals an event. */
  bool (*event_pending)(void *ctx);
  /** Milliseconds, monotonic. */
  uint32_t (*now_ms)(void *ctx);
  /** An event frame was received. Called inside request() and fetch_events(). */
  void (*on_event)(void *ctx, const sensei_LpwanEvent *evt);
};

struct lpwan_master {
  const struct lpwan_master_ops *ops;
  void *ctx;
  uint32_t seq;
  uint32_t ready_timeout_ms;
  uint8_t retries;
  bool rx_error;        /* the last received frame was bad: flag it in the next frame */
  bool peer_error;      /* the slave flagged our last frame as unreadable */
  bool ack_pending;     /* confirm the last message taken, in the next frame sent */
  bool ack_tog;         /* toggle bit of the message to confirm */
  bool tx_ack;          /* the frame in tx carries an ACK: pending until it has gone out */
  bool have_tog;        /* last_tog is valid (false after init and reset: the next message is taken) */
  bool last_tog;        /* toggle bit of the last message taken */
  uint8_t version_errs; /* frames in a row with another SPI frame version */
  uint8_t tx[LPWAN_XFER_LEN];
  uint8_t rx[LPWAN_XFER_LEN];

  /** Requests that failed in a row (no answer, SPI error, protocol mismatch). 0 after a success. */
  uint32_t failures;
  uint32_t n_transfers, n_bad_frames, n_retries, n_events;
};

void lpwan_master_init(struct lpwan_master *m, const struct lpwan_master_ops *ops, void *ctx);

/**
 * The slave restarted (shield power cycle, nRF9151 reset): forget the toggle and ACK state, so the
 * first message of the new session is taken. Keeps seq and the counters.
 */
void lpwan_master_reset(struct lpwan_master *m);

/** Clear the transmit buffer (it holds the last request, for example LoRaWAN keys). */
void lpwan_master_wipe(struct lpwan_master *m);

/**
 * Send a request (seq is assigned here) and wait up to timeout_ms for its response. Events that
 * arrive meanwhile go to on_event. Returns 0 (rsp filled, rsp->result holds the slave's verdict),
 * -ETIMEDOUT, -EPROTONOSUPPORT (the slave speaks another SPI frame version) or another negative
 * errno. `failures` counts the requests that failed in a row.
 */
int lpwan_master_request(struct lpwan_master *m, sensei_LpwanRequest *req, sensei_LpwanResponse *rsp,
                         uint32_t timeout_ms);

/** Fetch events while the slave signals them (at most max_frames transfers). Returns the number
 *  of events delivered, or a negative errno. */
int lpwan_master_fetch_events(struct lpwan_master *m, unsigned max_frames);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_MASTER_H_ */
