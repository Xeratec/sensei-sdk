/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_priv.h
 * @brief Calls between lpwan_client.c and lpwan_shell.c.
 */

#ifndef SENSEI_LPWAN_PRIV_H_
#define SENSEI_LPWAN_PRIV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"
#include "sensei_lpwan/lpwan_link.h"

struct lpwan_info {
  lpwan_state_t state;
  lpwan_tech_t tech;
  bool shield_on;
  bool link_up;
  bool lte_mode_nb;
  uint32_t transfers, bad_frames, retries, events;
  uint32_t uplink_ok, uplink_fail;
  int last_result; /* LpwanResult of the last request, or a negative errno */
  char fw[24];
  char modem[24];
  bool provisioned_pending;
  bool proto_mismatch; /* the shield speaks another protocol version */
  uint32_t recoveries; /* shield power cycles after a lost link */
  uint32_t failures;   /* requests that failed in a row */
  uint32_t lte_acked;  /* LTE sends the server answered (one-response RAI) */
  uint32_t lte_noack;  /* ... and those that were sent without an answer (unconfirmed) */
};

void sensei_lpwan_get_info(struct lpwan_info *info);

/** Ask the client thread to connect a technology (async). */
int sensei_lpwan_cmd_connect(sensei_Radio radio);
int sensei_lpwan_cmd_disconnect(void);
int sensei_lpwan_cmd_power(bool on);

/** True while an exchange holds the shared SPI_A bus. */
bool sensei_lpwan_spi_in_use(void);

#endif /* SENSEI_LPWAN_PRIV_H_ */
