/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_wdt.h
 * @brief Hardware watchdog (devicetree alias watchdog0), fed only while the application is healthy.
 */

#ifndef SENSEI_WDT_H_
#define SENSEI_WDT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the watchdog with a timeout of timeout_s seconds. A delayable work item feeds it every
 * CONFIG_SENSEI_WDT_FEED_PERIOD_MS, but only if healthy() returns true (NULL: always). The SoC resets when the
 * feed stops. The watchdog pauses while a debugger halts the CPU.
 *
 * @param timeout_s Timeout in seconds. Must be longer than the feed period.
 * @param healthy   Called from the work item. May be NULL.
 * @param wq        Work queue that runs the feed (NULL: the system work queue).
 * @return 0, or a negative errno if the watchdog is not ready or the setup fails.
 */
int sensei_wdt_start(uint32_t timeout_s, bool (*healthy)(void), struct k_work_q *wq);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_WDT_H_ */
