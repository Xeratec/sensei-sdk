/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_lora.h
 * @brief LoRaWAN payload limits. Pure functions.
 */

#ifndef SENSEI_LPWAN_LORA_H_
#define SENSEI_LPWAN_LORA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Largest application payload of one uplink in EU868 (LoRaWAN RP002-1.0.4, repeater compatible, no
 * MAC commands in FOpts): DR0 to DR2 51 B, DR3 115 B, DR4 to DR7 222 B. A data rate above 7 is not an
 * EU868 rate: the smallest value is returned. With `adr` the network may move the device to any
 * rate, so the DR0 value is returned (the one that always fits).
 */
uint32_t lpwan_lora_max_payload(uint8_t data_rate, bool adr);

/** Margin when the LR1121 owes MAC answers: the network may add more before the next uplink (FOpts: 15 B). */
#define LPWAN_LORA_MAC_MARGIN 15u
/** LoRaWAN frame bytes around the application payload (MHDR, DevAddr, FCtrl, FCnt, FPort, MIC = 13) plus
 *  15 B of FOpts, for the airtime estimate. */
#define LPWAN_LORA_PHY_OVERHEAD 28u
/** Receive windows after a LoRaWAN uplink (RX1 after 1 s, RX2 after 2 s, plus the window), ms. */
#define LPWAN_LORA_RX_MS 3000u
/** Longest time a send may take before it is given up, ms. */
#define LPWAN_SEND_MAX_MS 600000u

/**
 * Bytes per chunk (the fragment header included) for the next LoRa uplink. `next_max` is what the LR1121
 * reported for it (0: not known): it counts the MAC answers the LR1121 owes. If it is below the nominal
 * value, MAC traffic is flowing: LPWAN_LORA_MAC_MARGIN more are left free. Never below 4.
 */
uint32_t lpwan_lora_chunk_max(uint8_t data_rate, bool adr, uint32_t next_max);

/** Time on air of a LoRaWAN uplink of `phy_len` bytes (EU868 data rates 0 to 7; 8 and up: as DR0), ms, rounded up. */
uint32_t lpwan_lora_airtime_ms(uint8_t data_rate, uint32_t phy_len);

/**
 * Time from the send request to tx_done for an application payload of `app_len` bytes: the duty-cycle wait the
 * LR1121 reports, the airtime (DR0 with `adr`: the network may lower the rate) and the receive windows. At most
 * LPWAN_SEND_MAX_MS.
 */
uint32_t lpwan_lora_expected_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t duty_wait_ms);

/**
 * Time limit of a send: `expected_ms` (0: not known) plus `extra_ms`, not below `floor_ms`. The nRF9151 uses a
 * small `extra_ms`, the nRF5340 a larger one, so the nRF9151 ends a stuck send first.
 */
uint32_t lpwan_send_limit_ms(uint32_t expected_ms, uint32_t floor_ms, uint32_t extra_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_LORA_H_ */
