/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_lora.h"

uint32_t lpwan_lora_max_payload(uint8_t data_rate, bool adr) {
  static const uint8_t eu868[] = {51, 51, 51, 115, 222, 222, 222, 222};

  if (adr || data_rate >= sizeof(eu868)) {
    return eu868[0];
  }
  return eu868[data_rate];
}

uint32_t lpwan_lora_chunk_max(uint8_t data_rate, bool adr, uint32_t next_max) {
  uint32_t nominal = lpwan_lora_max_payload(data_rate, adr);

  if (next_max == 0 || next_max >= nominal) {
    return nominal;
  }
  /* The LR1121 owes MAC answers: they take room now, and the network may ask for more. */
  return next_max > LPWAN_LORA_MAC_MARGIN + 4u ? next_max - LPWAN_LORA_MAC_MARGIN : 4u;
}

uint32_t lpwan_lora_airtime_ms(uint8_t data_rate, uint32_t phy_len) {
  static const uint8_t sf[] = {12, 11, 10, 9, 8, 7, 7};
  uint32_t bw, tsym_us, de, sfv, n_pl;
  int32_t num;

  if (data_rate == 7) { /* FSK 50 kbit/s: preamble 5, sync 3, length 1, payload, CRC 2 (160 us per byte) */
    return ((11u + phy_len) * 160u + 999u) / 1000u;
  }
  if (data_rate > 6) {
    data_rate = 0;
  }
  sfv = sf[data_rate];
  bw = data_rate == 6 ? 250000u : 125000u;
  tsym_us = (1u << sfv) * 1000000u / bw;
  de = (sfv >= 11 && bw == 125000u) ? 1u : 0u;
  /* Semtech AN1200.13: explicit header, CRC on, coding rate 4/5, 8 preamble symbols. */
  num = (int32_t)(8u * phy_len) - 4 * (int32_t)sfv + 28 + 16;
  n_pl = 8u;
  if (num > 0) {
    uint32_t den = 4u * (sfv - 2u * de);

    n_pl += (((uint32_t)num + den - 1u) / den) * 5u;
  }
  /* (8 + 4.25 + n_pl) symbols, in quarters to stay in integers */
  return (uint32_t)(((uint64_t)(49u + 4u * n_pl) * tsym_us / 4u + 999u) / 1000u);
}

uint32_t lpwan_lora_expected_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t duty_wait_ms) {
  uint64_t t = (uint64_t)duty_wait_ms + lpwan_lora_airtime_ms(adr ? 0 : data_rate, app_len + LPWAN_LORA_PHY_OVERHEAD) +
               LPWAN_LORA_RX_MS;

  return t > LPWAN_SEND_MAX_MS ? LPWAN_SEND_MAX_MS : (uint32_t)t;
}

uint32_t lpwan_send_limit_ms(uint32_t expected_ms, uint32_t floor_ms, uint32_t extra_ms) {
  uint32_t t = (expected_ms > LPWAN_SEND_MAX_MS ? LPWAN_SEND_MAX_MS : expected_ms) + extra_ms;

  return t > floor_ms ? t : floor_ms;
}
