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

/** Reset cause, boot count and, once, the last fault into a LpwanStatus. */
void diag_fill_status(sensei_LpwanStatus *st);

#endif /* LPWAN_SHIELD_DIAG_H_ */
