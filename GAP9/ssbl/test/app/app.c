/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Test app of the SSBL: says which slot it is (APP_ID) and which slot the SSBL's hand-off record names, and ends. */

#include <pmsis.h>

#include "sensei_gap9/gap9_slot.h"

#ifndef APP_ID
#define APP_ID "?"
#endif

/* Some data, so that the app has more than one section with a CRC. */
static const char note[] = "SENSEI SSBL test app, slot " APP_ID;
static volatile int counter = 41;

int main(void) {
  int h = sensei_g9_handoff_decode((const volatile uint32_t *)SENSEI_G9_HANDOFF_ADDR);

  counter++;
  printf("%s running (%d), hand-off %c\n", note, counter, h < 0 ? '-' : 'A' + h);
  pmsis_exit(0);
  return 0;
}
