/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file service.h
 * @brief What the LTE and LoRa backends use to report to the master: events and the shared
 *        state machine.
 */

#ifndef SENSEI_NRF9151_SERVICE_H_
#define SENSEI_NRF9151_SERVICE_H_

#include <stdint.h>

#include "sensei_lpwan/lpwan_link.h"

/** Update the state machine and queue the event for the master. Safe from any thread. */
void svc_emit(const sensei_LpwanEvent *evt);

void svc_emit_radio(pb_size_t tag, sensei_Radio radio); /* joined, connected, disconnected */
/** The result of a send. Each seq gets one: a second tx_done for it (after the watchdog) is dropped.
 *  next_max_payload: LoRa, bytes the next uplink takes, 0: not known. */
void svc_emit_tx_done(uint32_t seq, bool ok, uint32_t attempts, uint32_t next_max_payload);
/** tx_done of an LTE send that may wait for the answer of the server: acked says it came, unconfirmed that the
 *  datagram went out and the wait ended without an answer (ok is true then). */
void svc_emit_tx_done_ack(uint32_t seq, bool ok, uint32_t attempts, bool acked, bool unconfirmed);
void svc_emit_downlink(sensei_Radio radio, uint32_t port, const uint8_t *data, size_t len);
void svc_emit_time(sensei_Radio radio, uint32_t unix_s);
void svc_emit_error(sensei_LpwanResult code, const char *msg);

/** Current state (for status and logs). */
lpwan_state_t svc_state(void);
lpwan_tech_t svc_tech(void);

#endif /* SENSEI_NRF9151_SERVICE_H_ */
