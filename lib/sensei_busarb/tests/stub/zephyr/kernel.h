/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The few Zephyr kernel calls bus_arb_zephyr.c uses, with a clock the test sets. */

#ifndef STUB_ZEPHYR_KERNEL_H_
#define STUB_ZEPHYR_KERNEL_H_

#include <stddef.h>
#include <stdint.h>

struct k_spinlock {
  int unused;
};
typedef int k_spinlock_key_t;

extern int64_t stub_uptime_ms;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {
  (void)l;
  return 0;
}
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k) {
  (void)l;
  (void)k;
}
static inline int64_t k_uptime_get(void) { return stub_uptime_ms; }
static inline int32_t k_msleep(int32_t ms) {
  stub_uptime_ms += ms;
  return 0;
}

#endif /* STUB_ZEPHYR_KERNEL_H_ */
