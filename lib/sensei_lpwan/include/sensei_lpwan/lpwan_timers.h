/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_timers.h
 * @brief Encoding of the LTE power saving timers (3GPP GPRS timer 2 and 3, eDRX and paging time
 *        window) as the bit strings the modem library wants. Pure functions.
 */

#ifndef SENSEI_LPWAN_TIMERS_H_
#define SENSEI_LPWAN_TIMERS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Periodic TAU (GPRS timer 3, T3412 extended) for `seconds`, as 8 characters of '0'/'1'. The value
 * is rounded up to the next representable one (2 s to 320 h). Returns 0 or -1 if out of range.
 */
int lpwan_encode_tau(uint32_t seconds, char out[9]);

/** Active time (GPRS timer 2, T3324) for `seconds`, rounded up (2 s to 186 min). 8 characters. */
int lpwan_encode_active(uint32_t seconds, char out[9]);

/** eDRX cycle as 4 characters, the longest cycle that is not above `ms`. nb_iot selects the table.
 */
int lpwan_encode_edrx(uint32_t ms, bool nb_iot, char out[5]);

/** Paging time window as 4 characters, the longest window not above `ms`. */
int lpwan_encode_ptw(uint32_t ms, bool nb_iot, char out[5]);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_TIMERS_H_ */
