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
