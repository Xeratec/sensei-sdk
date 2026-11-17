/*
 * ----------------------------------------------------------------------
 *
 * File: main.c
 *
 * Copyright (C) 2025 ETH Zurich and University of Bologna
 *
 * Based on the semester project of Ahmed Malanovic (ETH Zurich, 2025).
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "crypto/sensei_crypto.h"
#include "example.h"

int main(void) {
  printk("SENSEI crypto example\n");

  sensei_crypto_status_t st = sensei_crypto_init();
  if (st != SENSEI_CRYPTO_OK) {
    printk("sensei_crypto_init failed: %d\n", (int)st);
    return 0;
  }

  int failed = run_tests();
  if (failed == 0) {
    printk("ALL TESTS PASSED\n");
  } else {
    printk("TESTS FAILED: %d\n", failed);
  }

  run_benchmarks();

  printk("Done\n");
  return 0;
}
