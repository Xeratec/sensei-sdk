/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Test app of the SSBL: says which slot it is (APP_ID) and ends. */

#include <pmsis.h>

#ifndef APP_ID
#define APP_ID "?"
#endif

/* Some data, so that the app has more than one section with a CRC. */
static const char note[] = "SENSEI SSBL test app, slot " APP_ID;
static volatile int counter = 41;

int main(void) {
  counter++;
  printf("%s running (%d)\n", note, counter);
  pmsis_exit(0);
  return 0;
}
