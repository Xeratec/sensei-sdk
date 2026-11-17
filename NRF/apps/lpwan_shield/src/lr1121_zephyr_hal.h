/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lr1121_zephyr_hal.h
 * @brief The LR1121 context (devicetree specs) for the Modem-E driver.
 */

#ifndef SENSEI_LR1121_ZEPHYR_HAL_H_
#define SENSEI_LR1121_ZEPHYR_HAL_H_

#include "lr1121_modem_hal.h"

/** The one LR1121 on the shield. Pass it as `context` to the driver calls. */
extern struct lr1121_context lr1121_ctx;

/** Check the devicetree devices and park the pins (radio held in reset). */
int lr1121_hal_init(void);

#endif /* SENSEI_LR1121_ZEPHYR_HAL_H_ */
