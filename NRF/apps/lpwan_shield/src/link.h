/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file link.h
 * @brief SPI slave transport to the nRF5340: SPIS, BUSY/EVENT/ACK lines and the link thread.
 */

#ifndef SENSEI_NRF9151_LINK_H_
#define SENSEI_NRF9151_LINK_H_

#include "sensei_lpwan/link_slave.h"

/** Start the link thread. The slave answers requests from now on, also while the modem boots. */
int link_start(link_slave_request_fn on_request, void *user);

/** Respond to the request being handled (call from on_request). */
int link_respond(const sensei_LpwanResponse *rsp);

/** Queue an event for the master. Safe from any thread. */
int link_push_event(const sensei_LpwanEvent *evt);

/** Events dropped since start because the queue was full. */
uint32_t link_dropped_events(void);

/** Feed the watchdog of the link thread (a request that takes long, such as a flash erase, calls it). */
void link_feed_watchdog(void);

/** Level of the ACK line (the master holds it high while the link is open). */
bool link_master_present(void);

#endif /* SENSEI_NRF9151_LINK_H_ */
