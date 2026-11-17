/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lte.h
 * @brief LTE-M / NB-IoT backend: nRF modem library, UDP or TCP socket to the configured server.
 *
 * All calls return at once. Connect, send and receive run in worker threads and report with
 * events (svc_emit_*). The modem library starts at the first connect, so the modem stays off until
 * LTE is used.
 */

#ifndef SENSEI_NRF9151_LTE_H_
#define SENSEI_NRF9151_LTE_H_

#include "sensei_lpwan/lpwan_link.h"

int lte_init(void);
sensei_LpwanResult lte_configure(const sensei_LteConfigure *cfg);
sensei_LpwanResult lte_connect(void);
sensei_LpwanResult lte_disconnect(void);
sensei_LpwanResult lte_send(uint32_t seq, uint32_t port, const uint8_t *data, size_t len);
/** Network time is delivered as an event as soon as it is known. */
sensei_LpwanResult lte_request_time(void);
/** deep = 0: keep the registration (the modem sleeps in PSM). deep = 1: modem off. */
sensei_LpwanResult lte_sleep(uint32_t deep);
void lte_fill_status(sensei_LpwanStatus *st);
void lte_modem_string(char *out, size_t cap);

#endif /* SENSEI_NRF9151_LTE_H_ */
