/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <stddef.h>
#include <stdio.h>

#include "sensei_gas_index.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

/* Number of the first call that returns a valid index with constant raw ticks, or -1. */
static int first_valid(float interval_s, int calls) {
  struct sensei_gas_index g;
  int32_t voc = 0;
  int32_t nox = 0;

  sensei_gas_index_init(&g, interval_s);
  for (int i = 1; i <= calls; i++) {
    if (sensei_gas_index_process(&g, 30000, 15000, &voc, &nox)) {
      return i;
    }
  }
  return -1;
}

static void test_blackout(void) {
  /* The blackout is 45 s of algorithm time: 46 calls at 1 s, one call at 60 s; the first index follows. */
  CHECK(first_valid(1.0f, 100) == 47);
  CHECK(first_valid(60.0f, 10) == 2);
}

static void test_values(void) {
  struct sensei_gas_index g;
  int32_t voc = -1;
  int32_t nox = -1;

  sensei_gas_index_init(&g, 1.0f);
  CHECK(!sensei_gas_index_process(&g, 30000, 15000, &voc, &nox));
  CHECK(voc == 0 && nox == 0);
  for (int i = 0; i < 200; i++) {
    sensei_gas_index_process(&g, 30000, 15000, &voc, &nox);
  }
  /* Constant ticks: the VOC index settles at its reference value (100), the NOx index at 1. */
  CHECK(voc >= 80 && voc <= 120);
  CHECK(nox >= 1 && nox <= 20);
  /* NULL outputs are allowed. */
  CHECK(sensei_gas_index_process(&g, 30000, 15000, NULL, NULL));
}

static void test_restart(void) {
  struct sensei_gas_index g;
  int32_t voc;
  int32_t nox;

  sensei_gas_index_init(&g, 1.0f);
  for (int i = 0; i < 100; i++) {
    sensei_gas_index_process(&g, 30000, 15000, &voc, &nox);
  }
  CHECK(sensei_gas_index_process(&g, 30000, 15000, &voc, &nox));
  sensei_gas_index_init(&g, 1.0f);
  CHECK(!sensei_gas_index_process(&g, 30000, 15000, &voc, &nox));
}

int main(void) {
  test_blackout();
  test_values();
  test_restart();
  if (fails) {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("sensei_gas_index: all checks passed\n");
  return 0;
}
