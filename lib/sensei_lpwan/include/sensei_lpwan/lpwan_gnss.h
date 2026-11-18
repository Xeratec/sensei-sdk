/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_gnss.h
 * @brief Rules of the GNSS of the shield's nRF9151, shared by the shield app and the nRF5340 client. No Zephyr
 *        dependency, host tested (tests/test_lpwan_gnss.c).
 *
 * One fix at a time. The node asks with `gnss_fix` (LpwanRequest) and gets the result as the `gnss_result` event
 * (LpwanEvent). The request is answered at once: LPWAN_OK (the shield started), LPWAN_BUSY (not now), LPWAN_INVALID
 * (bad timeout), LPWAN_UNSUPPORTED (a shield without the feature).
 *
 * Fix and LTE (the rule the shield and the client follow):
 * - The modem gives LTE priority. With LTE registered the GNSS runs in the gaps (PSM, eDRX, idle) and pauses while the
 *   radio is connected, so a fix takes longer and may end "blocked", but it never delays an LTE send.
 * - The changes of the modem's functional mode (connect, disconnect, configure, deep sleep) and the fix exclude each
 *   other. A fix is refused (BUSY) while LTE connects, sends or disconnects. While a fix runs the shield refuses
 *   those requests (BUSY), and the client keeps LTE out of the way: radio_ready(LTE) is false and an LTE send gives
 *   -EAGAIN until the fix has ended, then on_up follows.
 * - LoRa is not touched by a fix.
 */

#ifndef SENSEI_LPWAN_GNSS_H_
#define SENSEI_LPWAN_GNSS_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_lpwan/lpwan_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Time of a fix attempt when the request says 0, and the longest the request may ask, s. */
#define LPWAN_GNSS_TIMEOUT_DEFAULT_S 120u
#define LPWAN_GNSS_TIMEOUT_MAX_S 900u
/** Time the client waits for the result after the attempt time: the modem library starts, the GNSS stops, s. */
#define LPWAN_GNSS_MARGIN_S 20u

/** The time of an attempt for a request: 0 gives the default; more than the maximum is refused (false). */
bool lpwan_gnss_timeout(uint32_t request_s, uint32_t *out_s);

/** How long the client waits for the result event after it sent the request, ms. */
uint32_t lpwan_gnss_wait_ms(uint32_t timeout_s);

/** Admission of a fix request: BUSY while a fix runs, or while LTE connects, sends or disconnects; else OK. */
sensei_LpwanResult lpwan_gnss_admit(const lpwan_sm_t *sm, bool fix_running);

/** True if, while a fix runs, the shield refuses this request (BUSY): everything that changes the functional mode of
 *  the modem or sends over LTE. */
bool lpwan_gnss_refuses(const sensei_LpwanRequest *req, const lpwan_sm_t *sm);

/** How an attempt that gave no fix ended: the modem kept GNSS blocked (or gave it no window) at the end, else it ran
 *  out of time. A cancel, an error and a fix are told apart by the caller. */
sensei_GnssFixStatus lpwan_gnss_end_status(bool fix, bool cancelled, bool error, bool blocked_at_end);

/** UTC to Unix seconds. 0 for a date outside 2020 ... 2099 or a field out of range (the receiver has no time). */
uint32_t lpwan_gnss_utc_to_unix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                uint8_t second);

/** Horizontal accuracy in m from the receiver's estimate (a float): rounded up, 0 for a bad value, capped at 65535. */
uint32_t lpwan_gnss_acc_m(float acc);

/** Degrees to degrees x 1e7, rounded, clamped to +-limit_deg (90 or 180). */
int32_t lpwan_gnss_deg_e7(double deg, int limit_deg);

/** Name of a status for logs. */
const char *lpwan_gnss_status_name(sensei_GnssFixStatus s);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_GNSS_H_ */
