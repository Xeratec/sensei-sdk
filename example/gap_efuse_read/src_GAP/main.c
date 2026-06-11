/*
 * ----------------------------------------------------------------------
 *
 * File: main.c
 *
 * Last edited: 24.11.2025
 *
 * Copyright (C) 2025, ETH Zurich and University of Bologna.
 *
 * Authors:
 * - Philip Wiese (wiesep@iis.ee.ethz.ch), ETH Zurich
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

// This example shows how to READ the GAP9 efuses.
// Reading is non-destructive and can be done freely.
//
// NOTE: *Programming* an efuse (not done here) is a permanent, one-time
// operation: once programmed, the same value is read back even after power-up.
// Some efuses are reserved by the ROM to specify the boot mode, so consult the
// specification before ever programming any of them.

#include <pmsis.h>

#define FUSER_REG_NUM (128)

unsigned int fuser_read[FUSER_REG_NUM];

int main(void) {
  printf("Fuse Reader Start\n");

  if (pi_efuse_ioctl(PI_EFUSE_IOCTL_READ_START, NULL)) {
    printf("Failed to start efuse read\n");
    return -1;
  }

  for (unsigned int i = 0; i < FUSER_REG_NUM; i++) {
    printf("Read efuse 0x%X = 0x%X\n", i, pi_efuse_value_get(i));
  }

  pi_efuse_ioctl(PI_EFUSE_IOCTL_CLOSE, NULL);

  return 0;
}
