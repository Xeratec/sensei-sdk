/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* LoRa payload limit from the data rate (EU868). */

#include <stdio.h>

#include "sensei_lpwan/lpwan_lora.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

int main(void) {
  /* DR0 to DR2: 51 B, DR3: 115 B, DR4 and up: 222 B */
  CHECK(lpwan_lora_max_payload(0, false) == 51);
  CHECK(lpwan_lora_max_payload(2, false) == 51);
  CHECK(lpwan_lora_max_payload(3, false) == 115);
  CHECK(lpwan_lora_max_payload(4, false) == 222);
  CHECK(lpwan_lora_max_payload(5, false) == 222);
  CHECK(lpwan_lora_max_payload(7, false) == 222);
  /* not an EU868 rate: the safe value */
  CHECK(lpwan_lora_max_payload(8, false) == 51);
  CHECK(lpwan_lora_max_payload(15, false) == 51);
  /* ADR: the network may lower the rate to DR0 whatever we configured */
  for (unsigned dr = 0; dr <= 15; dr++) {
    CHECK(lpwan_lora_max_payload((uint8_t)dr, true) == 51);
  }
  /* The node chunks by this value: a chunk of 3 B header and the payload must fit one uplink */
  CHECK(lpwan_lora_max_payload(5, false) <= 222);

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
