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

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_LORA_H_ */
