/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file dfu.h
 * @brief Update of the shield over the SPI link: the app goes into the second MCUboot slot, the modem firmware goes to
 *        the modem as a delta patch. The rules are in lib/sensei_lpwan (lpwan_dfu.h); this file is the flash, the
 *        modem and the trial boot behind them.
 */

#ifndef SENSEI_NRF9151_DFU_H_
#define SENSEI_NRF9151_DFU_H_

#include <stdbool.h>

#include "sensei_lpwan/lpwan_link.h"

#ifdef CONFIG_LPWAN_SHIELD_DFU

/** Start the trial supervision if the running image was just installed (call at the end of start-up). */
void dfu_init(void);

/** Handle a dfu_* request on the link thread. Fills rsp (payload dfu) and returns the result. */
sensei_LpwanResult dfu_handle(const sensei_LpwanRequest *req, sensei_LpwanResponse *rsp);

/** True while a transfer is open or an update runs: radios must stay idle. */
bool dfu_holds_link(void);

/** image_id, trial and dfu_targets of LpwanVersion. */
void dfu_fill_version(sensei_LpwanVersion *v);

#else

static inline void dfu_init(void) {}
static inline bool dfu_holds_link(void) { return false; }
static inline void dfu_fill_version(sensei_LpwanVersion *v) { (void)v; }

#endif

#endif /* SENSEI_NRF9151_DFU_H_ */
