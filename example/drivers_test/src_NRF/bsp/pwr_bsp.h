/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef PWR_BSP_H_
#define PWR_BSP_H_

/** @brief Hooks of the SDK power module. Both return a negative value on error. */
int pwr_bsp_init();
int pwr_bsp_start();

#endif /* PWR_BSP_H_ */
