/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Encoding of the LTE power saving timers (3GPP TS 24.008 coding). */

#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_timers.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static bool tau_is(uint32_t s, const char *want) {
  char b[9];

  return lpwan_encode_tau(s, b) == 0 && strcmp(b, want) == 0;
}

int main(void) {
  char b[9];

  /* T3412 extended: unit codes 011 = 2 s, 100 = 30 s, 101 = 1 min, 000 = 10 min, 001 = 1 h,
   * 010 = 10 h, 110 = 320 h. */
  /* The smallest unit in which the value fits in 5 bits is used, rounding up. */
  CHECK(tau_is(2, "01100001"));       /* 1 x 2 s */
  CHECK(tau_is(60, "01111110"));      /* 30 x 2 s */
  CHECK(tau_is(300, "10001010"));     /* 10 x 30 s */
  CHECK(tau_is(3600, "00000110"));    /* 6 x 10 min */
  CHECK(tau_is(86400, "00111000"));   /* 24 x 1 h */
  CHECK(tau_is(90000, "00111001"));   /* 25 x 1 h */
  CHECK(tau_is(1000000, "01011100")); /* 277.8 h rounded up to 28 x 10 h */
  CHECK(tau_is(1200000, "11000010")); /* 333 h: 10 h units do not fit, 2 x 320 h */
  /* beyond 31 x 320 h */
  CHECK(lpwan_encode_tau(40000000, b) != 0);

  /* T3324: 000 = 2 s, 001 = 1 min, 010 = 6 min */
  CHECK(lpwan_encode_active(0, b) == 0 && strcmp(b, "00000000") == 0);
  CHECK(lpwan_encode_active(6, b) == 0 && strcmp(b, "00000011") == 0);
  CHECK(lpwan_encode_active(120, b) == 0 && strcmp(b, "00100010") == 0);
  CHECK(lpwan_encode_active(3600, b) == 0 && strcmp(b, "01001010") == 0);
  CHECK(lpwan_encode_active(100000, b) != 0);

  char c[5];

  CHECK(lpwan_encode_edrx(5120, false, c) == 0 && strcmp(c, "0000") == 0);
  CHECK(lpwan_encode_edrx(81920, false, c) == 0 && strcmp(c, "0101") == 0);
  CHECK(lpwan_encode_edrx(100000, false, c) == 0 && strcmp(c, "0101") == 0);
  CHECK(lpwan_encode_edrx(10485760, false, c) == 0 && strcmp(c, "1111") == 0);
  CHECK(lpwan_encode_edrx(1000, false, c) != 0);
  CHECK(lpwan_encode_edrx(20480, true, c) == 0 && strcmp(c, "0000") == 0);
  CHECK(lpwan_encode_edrx(163840, true, c) == 0 && strcmp(c, "0011") == 0);
  CHECK(lpwan_encode_edrx(10485760, true, c) == 0 && strcmp(c, "1001") == 0);

  CHECK(lpwan_encode_ptw(1280, false, c) == 0 && strcmp(c, "0000") == 0);
  CHECK(lpwan_encode_ptw(5120, false, c) == 0 && strcmp(c, "0011") == 0);
  CHECK(lpwan_encode_ptw(99999, false, c) == 0 && strcmp(c, "1111") == 0);
  CHECK(lpwan_encode_ptw(2560, true, c) == 0 && strcmp(c, "0000") == 0);
  CHECK(lpwan_encode_ptw(100, true, c) != 0);

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
