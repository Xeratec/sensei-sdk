/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file diag.h
 * @brief Diagnostics of the shield: reset cause, boot count and the last fatal error (SDK NRF/system/diag) for
 * the nRF5340, which asks with get_status.
 */

#ifndef LPWAN_SHIELD_DIAG_H_
#define LPWAN_SHIELD_DIAG_H_

#include "sensei/lpwan.pb.h"

/** Take over the record of the previous run. Call once at start. */
void diag_init(void);

/** A fault of the modem core: keep it in the fault record (it survives the reset that follows). */
void diag_modem_fault(uint32_t reason, uint32_t pc);

/** Reset cause, boot count and the last fault into a LpwanStatus. The fault is in every status until the node acks it
 *  (diag_ack). It is kept in RAM only: a reset or a power cut of the shield loses it. */
void diag_fill_status(sensei_LpwanStatus *st);

/** The node has stored the fault of the last status (request diag_ack): forget it. */
void diag_ack(void);

#endif /* LPWAN_SHIELD_DIAG_H_ */
