/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The board hooks that an application gives NRF/system/pwr (here FFF fakes in main.c). */

#ifndef TEST_PWR_BSP_H_
#define TEST_PWR_BSP_H_

int pwr_bsp_init(void);
int pwr_bsp_start(void);

#endif /* TEST_PWR_BSP_H_ */
