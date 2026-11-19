/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_internal.h
 * @brief Internal interfaces between the parts of the Zephyr time sync layer. Not a public API.
 */

#ifndef SENSEI_TS_INTERNAL_H_
#define SENSEI_TS_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_egu.h>
#include <hal/nrf_rtc.h>
#include <hal/nrf_timer.h>

#include <nrfx_gpiote.h>

#include "sensei_timesync/timesync.h"
#include "sensei_timesync/ts_msg.h"

#define TS_RTC NRF_RTC_INST_GET(CONFIG_SENSEI_TIMESYNC_RTC)
#define TS_TIMER NRF_TIMER_INST_GET(CONFIG_SENSEI_TIMESYNC_TIMER)
#define TS_EGU NRF_EGU_INST_GET(CONFIG_SENSEI_TIMESYNC_EGU)

/* RTC CC and TIMER CC use: the same index on both. */
#define TS_CC_PPS 0
#define TS_CC_NOW 1
#define TS_CC_CAPTURE0 2

/* EGU channels. */
#define TS_EGU_CH_NOW 0
#define TS_EGU_CH_PPS 1
#define TS_EGU_CH_IPC 2
#define TS_EGU_CH_CAP0 3 /* + capture channel */

#ifdef CONFIG_SENSEI_TIMESYNC_TIMER_16MHZ
#define TS_TIMER_PRESCALER 0
/* TIMER counts per RTC tick (30517.578 ns), rounded down. */
#define TS_TICK_COUNTS 488
#else
#define TS_TIMER_PRESCALER 4
#define TS_TICK_COUNTS 30
#endif

/* ---- ts_timebase.c ---- */

/** Local time in ns now. 0 while the mirror does not run. Safe from any context. */
uint64_t ts_tb_local_ns(void);

/** Local time of a hardware capture (RTC CC and TIMER CC captured by the same event). Call within 512 s. */
uint64_t ts_tb_capture_to_ns(uint32_t rtc_cc, uint32_t timer_cc);

/** Split a local time into a full RTC tick count and TIMER counts after the tick, rounded to the nearest count. */
void ts_tb_split(uint64_t local_ns, uint64_t *ticks, uint32_t *counts);

/** Nanoseconds of a full RTC tick count. */
uint64_t ts_tb_ticks_to_ns(uint64_t ticks);

/** Current full RTC tick count (overflow safe). */
uint64_t ts_tb_ticks_now(void);

/** Number of IPC triggers seen: each one restarted the mirror at 0 (net core restart). */
uint32_t ts_tb_mirror_starts(void);

/** Set a function called from the EGU interrupt when the mirror starts (IPC trigger). */
void ts_tb_running_cb_set(void (*cb)(void));

/** True if the network core was released before the init of this module (the IPC trigger may have been missed). */
bool ts_tb_net_started_early(void);

/** PPS: called from the EGU interrupt when the toggle of the armed edge happened. */
void ts_pps_edge_isr(void);

/** PPS: (re)arm if there is a global time; stop and pull the pin low if not. */
void ts_pps_poke(void);

/** Resolve a GPIO pin to its GPIOTE instance (the one the Zephyr GPIO driver uses) and absolute pin number. */
int ts_gpio_resolve(const struct gpio_dt_spec *pin, nrfx_gpiote_t **g, nrfx_gpiote_pin_t *abs_pin);

/** Capture: called from the EGU interrupt when capture channel k was triggered. */
void ts_capture_isr(uint8_t k);

/* ---- ts_core.c ---- */

void ts_core_follower_pair(int64_t local_ns, int64_t ref_ns);
void ts_core_follower_anchor(uint16_t counter, uint64_t local_us);
void ts_core_follower_sample(const struct ts_msg *msg);
void ts_core_follower_param_update(void);
void ts_core_follower_link_lost(void);

#endif /* SENSEI_TS_INTERNAL_H_ */
