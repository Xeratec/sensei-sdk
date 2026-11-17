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
    uint32_t v = seconds / unit_s[i] + (seconds % unit_s[i] != 0u ? 1u : 0u); /* no overflow near UINT32_MAX */

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
  /* 3GPP TS 24.008 table 10.5.5.32: the code is the value of the 4 bits, not the position in the list.
   * LTE-M (WB-S1): all 16 values. NB-IoT (NB-S1): ten of them, the codes are the same as for LTE-M
   * (81.92 s is 0101 in both). */
  struct edrx {
    uint32_t ms;
    uint8_t code;
  };
  static const struct edrx ltem[] = {{5120, 0},     {10240, 1},    {20480, 2},    {40960, 3},
                                     {61440, 4},    {81920, 5},    {102400, 6},   {122880, 7},
                                     {143360, 8},   {163840, 9},   {327680, 10},  {655360, 11},
                                     {1310720, 12}, {2621440, 13}, {5242880, 14}, {10485760, 15}};
  static const struct edrx nb[] = {{20480, 2},   {40960, 3},    {81920, 5},    {163840, 9},   {327680, 10},
                                   {655360, 11}, {1310720, 12}, {2621440, 13}, {5242880, 14}, {10485760, 15}};
  const struct edrx *t = nb_iot ? nb : ltem;
  size_t n = nb_iot ? sizeof(nb) / sizeof(nb[0]) : sizeof(ltem) / sizeof(ltem[0]);
  size_t best = 0;

  if (ms < t[0].ms) {
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    if (t[i].ms <= ms) {
      best = i;
    }
  }
  bits(out, t[best].code, 4);
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
