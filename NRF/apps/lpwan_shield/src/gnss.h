/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gnss.h
 * @brief GNSS of the nRF9151: one fix on request (nrf_modem_gnss). The result is the gnss_result event.
 *
 * The rules (fix and LTE exclude each other for connect and disconnect, a send never waits for a fix) are in
 * sensei_lpwan/lpwan_gnss.h. No A-GNSS: the receiver starts from what the modem keeps in its own memory.
 */

#ifndef SENSEI_NRF9151_GNSS_H_
#define SENSEI_NRF9151_GNSS_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_lpwan/lpwan_link.h"

int gnss_init(void);

/** Start a fix. seq is the request's, the result event carries it. timeout_s: 0 is the default, at most 900.
 *  Returns at once: OK, BUSY (a fix runs, or LTE is busy), INVALID (bad timeout). The checks against the state of
 *  the radios are made by the caller (service.c, lpwan_gnss_admit). */
sensei_LpwanResult gnss_start(uint32_t seq, uint32_t timeout_s);

/** End the fix that runs, if any: the result event follows with status CANCELLED. */
sensei_LpwanResult gnss_cancel(void);

/** True from the start of a fix until its result event is queued. */
bool gnss_active(void);

/** Fill LpwanStatus gnss_attempts, gnss_fixes and gnss_on_ms. */
void gnss_fill_status(sensei_LpwanStatus *st);

#endif /* SENSEI_NRF9151_GNSS_H_ */
