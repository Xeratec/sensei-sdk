/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 * - Serge Wüest <swueest@ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

#include "sensei_lpwan/lpwan_timers.h"

#include <stddef.h>

static void bits(char *out, unsigned value, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    out[i] = (value >> (n - 1 - i)) & 1u ? '1' : '0';
  }
  out[n] = '\0';
}

/* Pick the smallest unit in which `seconds` fits in the 5-bit value, rounding up. */
static int encode_timer(uint32_t seconds, const uint32_t *unit_s, const uint8_t *unit_code, size_t n, char out[9]) {
  for (size_t i = 0; i < n; i++) {
    uint32_t v = (seconds + unit_s[i] - 1u) / unit_s[i];

    if (v <= 31u) {
      bits(out, (unsigned)unit_code[i] << 5 | v, 8);
      return 0;
    }
  }
  return -1;
}

int lpwan_encode_tau(uint32_t seconds, char out[9]) {
  /* T3412 extended: 2 s (011), 30 s (100), 1 min (101), 10 min (000), 1 h (001), 10 h (010),
   * 320 h (110). Ordered by unit size. */
  static const uint32_t unit_s[] = {2, 30, 60, 600, 3600, 36000, 1152000};
  static const uint8_t code[] = {3, 4, 5, 0, 1, 2, 6};

  return encode_timer(seconds, unit_s, code, sizeof(unit_s) / sizeof(unit_s[0]), out);
}

int lpwan_encode_active(uint32_t seconds, char out[9]) {
  /* T3324: 2 s (000), 1 min (001), 6 min (010). */
  static const uint32_t unit_s[] = {2, 60, 360};
  static const uint8_t code[] = {0, 1, 2};

  return encode_timer(seconds, unit_s, code, sizeof(unit_s) / sizeof(unit_s[0]), out);
}

int lpwan_encode_edrx(uint32_t ms, bool nb_iot, char out[5]) {
  /* LTE-M: 5.12 s, 10.24 s, 20.48 s, 40.96 s, 61.44 s, 81.92 s, 102.4 s, 122.88 s, 143.36 s,
   * 163.84 s, 327.68 s, 655.36 s, 1310.72 s, 2621.44 s, 5242.88 s, 10485.76 s.
   * NB-IoT: 20.48 s doubling up to 10485.76 s (ten values). */
  static const uint32_t ltem[] = {5120,   10240,  20480,  40960,  61440,   81920,   102400,  122880,
                                  143360, 163840, 327680, 655360, 1310720, 2621440, 5242880, 10485760};
  static const uint32_t nb[] = {20480, 40960, 81920, 163840, 327680, 655360, 1310720, 2621440, 5242880, 10485760};
  const uint32_t *t = nb_iot ? nb : ltem;
  size_t n = nb_iot ? sizeof(nb) / sizeof(nb[0]) : sizeof(ltem) / sizeof(ltem[0]);

  if (ms < t[0]) {
    return -1;
  }
  size_t best = 0;

  for (size_t i = 0; i < n; i++) {
    if (t[i] <= ms) {
      best = i;
    }
  }
  bits(out, (unsigned)best, 4);
  return 0;
}

int lpwan_encode_ptw(uint32_t ms, bool nb_iot, char out[5]) {
  /* LTE-M: n * 1.28 s, n = 1 ... 16. NB-IoT: n * 2.56 s, n = 1 ... 16. Code = n - 1. */
  uint32_t step = nb_iot ? 2560u : 1280u;

  if (ms < step) {
    return -1;
  }
  uint32_t n = ms / step;

  if (n > 16u) {
    n = 16u;
  }
  bits(out, n - 1u, 4);
  return 0;
}
