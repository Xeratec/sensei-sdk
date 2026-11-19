/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Check macro and a small deterministic random number generator for the time sync tests. */

#ifndef TS_TEST_UTIL_H_
#define TS_TEST_UTIL_H_

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

/* xorshift64*: uniform in [0, 1). */
static inline double rng_uniform(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return (double)((rng_state * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

/* Gaussian with the given sigma (Box-Muller). */
static inline double rng_gauss(double sigma) {
  double u1 = rng_uniform(), u2 = rng_uniform();
  if (u1 < 1e-300) {
    u1 = 1e-300;
  }
  return sigma * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static inline int64_t i64abs(int64_t v) { return v < 0 ? -v : v; }

#endif /* TS_TEST_UTIL_H_ */
