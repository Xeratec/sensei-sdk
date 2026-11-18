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
/** rai: the flag of the client for this chunk (the last chunk of a message): RAI option and, for one response, the wait
 *  for the answer of the server; tx_done reports it. */
sensei_LpwanResult lte_send(uint32_t seq, uint32_t port, const uint8_t *data, size_t len, sensei_LteRai rai);
/** Connection evaluation (%CONEVAL): the result is the conn_eval event. Not supported without
 * CONFIG_LTE_LC_CONN_EVAL_MODULE. */
sensei_LpwanResult lte_eval(void);
/** Network time is delivered as an event as soon as it is known. */
sensei_LpwanResult lte_request_time(void);
/** deep = 0: keep the registration (the modem sleeps in PSM). deep = 1: modem off. */
sensei_LpwanResult lte_sleep(uint32_t deep);
void lte_fill_status(sensei_LpwanStatus *st);
void lte_modem_string(char *out, size_t cap);
/** Start the modem library if it is not running (idempotent). 0 or a negative errno. */
int lte_modem_init(void);
/** GNSS: start the modem library if needed and, unless LTE is on, set the system mode with GNSS (the modem takes it
 * only while off). With LTE on the mode was set at the connect, with GNSS in it. 0 or a negative errno. */
int lte_gnss_prepare(void);
/** The firmware update shut the modem library down and started it again (ok: it runs): read the version again. */
void lte_modem_restarted(bool ok);

#endif /* SENSEI_NRF9151_LTE_H_ */
