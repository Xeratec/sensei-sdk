/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lora.h
 * @brief LoRaWAN backend on the LR1121 (Semtech Modem-E firmware).
 *
 * All calls return at once; the LR1121 is driven by one worker thread that reports with events.
 * The LR1121 keeps the LoRaWAN session in its own flash. After a power cycle the backend reports
 * `joined` if the session is still valid, so the shield can be powered down between uplinks.
 */

#ifndef SENSEI_NRF9151_LORA_H_
#define SENSEI_NRF9151_LORA_H_

#include "sensei_lpwan/lpwan_link.h"

int lora_init(void);
sensei_LpwanResult lora_configure(const sensei_LoraConfigure *cfg);
sensei_LpwanResult lora_provision(const sensei_LoraProvision *keys);
sensei_LpwanResult lora_join(void);
/** What lora_send() tells the master (SendInfo). */
struct lora_send_info {
  uint32_t expected_ms; /* OK: time to tx_done. BUSY: time until the frame is out, or until a send may be accepted */
  uint32_t next_max;    /* payload the next uplink takes (0: not known), also on a refusal */
  uint32_t queued_seq;  /* BUSY: seq of the send whose frame the LR1121 holds (0: none) */
};

/**
 * Queue an uplink. Refused at once, before anything is queued: BUSY (the LR1121 holds a frame: `queued_seq`; the
 * duty-cycle wait is longer than a send may take: the time after which it fits; the next uplink has no room for data
 * and an empty uplink is sent to flush the MAC answers), INVALID (it does not fit the next uplink, `next_max` says
 * what does). On success `expected_ms` is the time to tx_done: duty-cycle wait of the LR1121, the transmissions
 * (NbTrans, repeats of a confirmed uplink) and their receive windows.
 */
sensei_LpwanResult lora_send(uint32_t seq, uint32_t port, bool confirmed, const uint8_t *data, size_t len,
                             struct lora_send_info *info);
/** The LR1121 holds a frame (or one is on its way). `*seq`: its send (0: none, for example the empty uplink of a
 *  flush), `*left_ms`: time until it should be out. Both may be NULL. */
bool lora_held(uint32_t *seq, uint32_t *left_ms);
/** True if the frame of send `seq` is held by the LR1121 (or queued for it). */
bool lora_holds_send(uint32_t seq);
/** Reset the LR1121 to drop a frame it holds for too long. The worker does it and reports tx_done(failed), an error and
 *  `disconnected`. Returns false if the worker could not be told: try again. */
bool lora_reset_stuck(void);
sensei_LpwanResult lora_request_time(void);
/** Park the radio: the session is kept. */
sensei_LpwanResult lora_leave(void);
sensei_LpwanResult lora_sleep(uint32_t deep);
void lora_fill_status(sensei_LpwanStatus *st);
void lora_modem_string(char *out, size_t cap);

#endif /* SENSEI_NRF9151_LORA_H_ */
