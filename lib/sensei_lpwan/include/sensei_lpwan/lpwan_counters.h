/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_counters.h
 * @brief What the LTE modem did, for the battery charge counter of the node: time in the RRC connected state, attaches
 *        and PSM entries since the shield started. Fed by the lte_lc events (RRC update, registration status, modem
 *        sleep). Plain C, no Zephyr, the caller serialises the calls.
 *
 * Times are the uptime in ms (32 bit, wrapping); a connected period must be shorter than 24 days.
 */

#ifndef SENSEI_LPWAN_COUNTERS_H_
#define SENSEI_LPWAN_COUNTERS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lpwan_counters {
  uint32_t rrc_ms;      /**< finished periods in the RRC connected state, ms */
  uint32_t attaches;    /**< registrations (not registered to registered) */
  uint32_t psm_entries; /**< times the modem went into PSM */
  bool rrc_on;
  uint32_t rrc_since;
  bool registered;
};

/** RRC mode changed (LTE_LC_EVT_RRC_UPDATE). Repeated events of the same mode do nothing. */
void lpwan_cnt_rrc(struct lpwan_counters *c, bool connected, uint32_t now_ms);

/** Registration status changed (LTE_LC_EVT_NW_REG_STATUS): a registration counts as an attach. Losing the registration
 *  ends a connected period (no idle event comes then). */
void lpwan_cnt_reg(struct lpwan_counters *c, bool registered, uint32_t now_ms);

/** The modem entered PSM (LTE_LC_EVT_MODEM_SLEEP_ENTER of type PSM). */
void lpwan_cnt_psm(struct lpwan_counters *c);

/** Time in the connected state including a period that is still running. */
uint32_t lpwan_cnt_rrc_ms(const struct lpwan_counters *c, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_COUNTERS_H_ */
