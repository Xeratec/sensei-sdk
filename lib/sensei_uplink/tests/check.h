/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_UPLINK_TEST_CHECK_H_
#define SENSEI_UPLINK_TEST_CHECK_H_

#include <stdio.h>

static int fails;

#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

#define DONE()                                                                                                         \
  do {                                                                                                                 \
    printf("%s: %s\n", __FILE__, fails ? "FAILED" : "ok");                                                             \
    return fails ? 1 : 0;                                                                                              \
  } while (0)

#endif
