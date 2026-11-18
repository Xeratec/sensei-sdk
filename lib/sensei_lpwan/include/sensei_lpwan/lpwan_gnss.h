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
 * - An LTE send goes through while a fix runs, on both sides: the shield takes it, the client does not hold it back.
 *   This needs LTE connected already.
 * - What changes the functional mode of the modem and the fix exclude each other: LTE connect, LTE release (disconnect,
 *   deep sleep), LTE configure, and the shield update. The connect would set functional mode 1 and then 30, which ends
 * a running fix. A fix is refused (BUSY) while LTE connects, sends or disconnects. While a fix runs the shield refuses
 *   those requests (BUSY). The client keeps them back: an LTE connect waits for the end of the fix, and the app is
 *   told by on_up(RADIO_BLE) then; an LTE release (disconnect, or a change to another radio) is remembered and runs
 *   when the fix has ended, before on_up; the update of the shield is refused (-EBUSY). A cancel of the fix is always
 *   possible, also during an update.
 * - A fix that ends with LTE not wanted leaves the modem in functional mode 0 (power off), so that it saves what the
 *   GNSS learned before the shield is cut. With LTE on it ends with mode 30 (GNSS off, LTE as it was).
 * - LoRa is not touched by a fix, except that a sleep request is refused for any radio.
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

/** True if, while a fix runs, the shield refuses this request (BUSY): what changes the functional mode of the modem
 *  (LTE connect, release, configure, sleep) and the start of a shield update. LTE sends are not in the list. */
bool lpwan_gnss_refuses(const sensei_LpwanRequest *req, const lpwan_sm_t *sm);

/** True for the requests that go on while a shield update holds the link: version, status, diag ack, LTE disconnect,
 *  the update's own requests and gnss_cancel (so that a fix does not run on without its owner). */
bool lpwan_allowed_during_dfu(const sensei_LpwanRequest *req);

/**
 * The checks of the shield for a request, in this order (service.c on_request calls it; the host test runs the same
 * code): an update holds the link and the request is not one of lpwan_allowed_during_dfu(): BUSY. A fix request:
 * lpwan_gnss_admit(). A fix runs and lpwan_gnss_refuses(): BUSY (before the state machine moves). Else
 * lpwan_sm_admit(), which moves the state machine.
 */
sensei_LpwanResult lpwan_admit_request(lpwan_sm_t *sm, const sensei_LpwanRequest *req, bool dfu_holds_link,
                                       bool fix_running);

/** How an attempt that gave no fix ended: the modem kept GNSS blocked (or gave it no window) at the end, else it ran
 *  out of time. A cancel, an error and a fix are told apart by the caller. */
sensei_GnssFixStatus lpwan_gnss_end_status(bool fix, bool cancelled, bool error, bool blocked_at_end);

/** UTC to Unix seconds. 0 for a date outside 2020 ... 2099 or a field out of range (the receiver has no time). */
uint32_t lpwan_gnss_utc_to_unix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                uint8_t second);

/** Like lpwan_gnss_utc_to_unix(), rounded to the nearest second with the milliseconds of the receiver (0 ... 999). */
uint32_t lpwan_gnss_utc_to_unix_ms(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                   uint8_t second, uint16_t ms);

/** The time of the fix moved forward to now: unix_time (0 stays 0) plus age_ms (the shield) plus delay_ms (the client:
 *  from the arrival of the result to the call), rounded to the nearest second. */
uint32_t lpwan_gnss_time_now(uint32_t unix_time, uint32_t age_ms, uint32_t delay_ms);

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
