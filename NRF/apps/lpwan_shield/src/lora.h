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
sensei_LpwanResult lora_send(uint32_t seq, uint32_t port, bool confirmed, const uint8_t *data, size_t len);
sensei_LpwanResult lora_request_time(void);
/** Park the radio: the session is kept. */
sensei_LpwanResult lora_leave(void);
sensei_LpwanResult lora_sleep(uint32_t deep);
void lora_fill_status(sensei_LpwanStatus *st);
void lora_modem_string(char *out, size_t cap);

#endif /* SENSEI_NRF9151_LORA_H_ */
