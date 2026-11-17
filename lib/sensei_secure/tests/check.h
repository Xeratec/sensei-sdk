/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_SECURE_TEST_CHECK_H
#define SENSEI_SECURE_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int check_failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

#define CHECK_DONE() (printf("%s\n", check_failures ? "FAILED" : "ok"), check_failures ? 1 : 0)

#endif
