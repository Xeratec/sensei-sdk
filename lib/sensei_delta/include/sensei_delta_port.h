/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Profiling port. Select one of SENSEI_DELTA_PORT_NRF5340 (DWT cycle counter),
 * SENSEI_DELTA_PORT_GAP9 (pi_perf) or SENSEI_DELTA_PORT_HOST (clock(), in clock ticks). Included by
 * sensei_delta.h.
 *
 * sensei_delta_prof_cycles[tag] accumulates the time spent in each tagged part. The application
 * calls sensei_delta_port_init() once and clears the array when it wants.
 * SENSEI_DELTA_MARK_HIGH/LOW are for an optional marker GPIO; define them before including to use.
 */

#ifndef SENSEI_DELTA_PORT_H
#define SENSEI_DELTA_PORT_H

#include <stdint.h>

#if defined(SENSEI_DELTA_PORT_NRF5340)

/* Cortex-M33 DWT, registers accessed directly. */
static inline void sensei_delta_port_init(void) {
  *(volatile uint32_t *)0xE000EDFCu |= (1u << 24); /* DEMCR.TRCENA */
  *(volatile uint32_t *)0xE0001004u = 0;           /* DWT_CYCCNT */
  *(volatile uint32_t *)0xE0001000u |= 1u;         /* DWT_CTRL.CYCCNTENA */
}
static inline uint32_t sensei_delta_port_cycles(void) { return *(volatile uint32_t *)0xE0001004u; }

#elif defined(SENSEI_DELTA_PORT_GAP9)

#include "pmsis.h"

static inline void sensei_delta_port_init(void) {
  pi_perf_conf(1u << PI_PERF_CYCLES);
  pi_perf_reset();
  pi_perf_start();
}
static inline uint32_t sensei_delta_port_cycles(void) { return pi_perf_read(PI_PERF_CYCLES); }

#elif defined(SENSEI_DELTA_PORT_HOST)

#include <time.h>

static inline void sensei_delta_port_init(void) {}
static inline uint32_t sensei_delta_port_cycles(void) { return (uint32_t)clock(); }

#else
#error "select a port: SENSEI_DELTA_PORT_NRF5340, _GAP9 or _HOST"
#endif

#ifndef SENSEI_DELTA_MARK_HIGH
#define SENSEI_DELTA_MARK_HIGH()
#define SENSEI_DELTA_MARK_LOW()
#endif

enum sensei_delta_prof_tag { SENSEI_DELTA_PROF_dpcm = 0, SENSEI_DELTA_PROF_coder, SENSEI_DELTA_PROF_N };
extern uint32_t sensei_delta_prof_cycles[SENSEI_DELTA_PROF_N];

#define SENSEI_DELTA_PROF_BEGIN(tag) uint32_t sensei_delta_t0_##tag = sensei_delta_port_cycles()
#define SENSEI_DELTA_PROF_END(tag)                                                                                     \
  (sensei_delta_prof_cycles[SENSEI_DELTA_PROF_##tag] += sensei_delta_port_cycles() - sensei_delta_t0_##tag)

#endif /* SENSEI_DELTA_PORT_H */
