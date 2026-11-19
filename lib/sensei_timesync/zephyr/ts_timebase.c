/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_timebase.c
 * @brief Application core mirror of the controller clock: RTC (cleared over IPC/DPPI when the network core
 *        starts its RTC) plus TIMER (cleared on every RTC tick) for the part below one tick.
 *
 * Resources: RTC CC0 PPS, CC1 software now-capture, CC2/CC3 edge capture; TIMER the same; EGU channel 0 triggers the
 * now-capture, channel 1 reports the PPS edge, channel 2 reports the IPC trigger.
 */

#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/onoff.h>

#include <hal/nrf_ipc.h>
#include <hal/nrf_reset.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx.h>

#include "ts_internal.h"

LOG_MODULE_REGISTER(ts_timebase, CONFIG_SENSEI_TIMESYNC_LOG_LEVEL);

#define RTC_BITS 24
#define RTC_MASK 0xFFFFFFu
/* 1e9 / 32768 ns = 1953125 / 64 ns per tick */
#define TICK_NUM 1953125ull
#define TICK_SHIFT 6

#ifdef CONFIG_SENSEI_TIMESYNC_TIMER_16MHZ
/* Counts at or above this after an RTC capture cannot be a valid sub-tick value (see ts_tb_capture_to_ns). */
#define COUNTS_GUARD TS_TICK_COUNTS
#else
#define COUNTS_GUARD (TS_TICK_COUNTS + 1)
#endif

/* Bounded wait for the RTC capture register update: 6 PCLK16M cycles (375 ns). One register read takes more than
 * 100 ns, so this is far more than needed and still limited to a few tens of microseconds. */
#define CAPTURE_WAIT_LOOPS 200

static volatile uint32_t rtc_ovf;
static volatile bool running;
static volatile uint32_t mirror_starts;
static bool net_early;
static void (*running_cb)(void);
static uint32_t capture_timeouts;

BUILD_ASSERT(CONFIG_SENSEI_TIMESYNC_RTC != 1, "RTC1 is the Zephyr system timer on the nRF5340 application core");

static inline uint64_t ticks_to_ns(uint64_t ticks) { return (ticks * TICK_NUM) >> TICK_SHIFT; }

uint64_t ts_tb_ticks_to_ns(uint64_t ticks) { return ticks_to_ns(ticks); }

static inline uint32_t counts_to_ns(uint32_t counts) {
#ifdef CONFIG_SENSEI_TIMESYNC_TIMER_16MHZ
  return counts * 125u / 2u;
#else
  return counts * 1000u;
#endif
}

/* Full tick count of the RTC counter read now. Interrupts must be locked. A pending overflow that the interrupt
 * has not counted yet is added when the counter is in its first half (an overflow that happened after the read
 * of the counter, with a counter near the top, is not). Also returns the 24 bit counter. */
static uint64_t ticks_now_locked(uint32_t *cnt_out) {
  uint32_t cnt = nrf_rtc_counter_get(TS_RTC);
  uint32_t ovf = rtc_ovf;

  if (nrf_rtc_event_check(TS_RTC, NRF_RTC_EVENT_OVERFLOW) && cnt < (RTC_MASK >> 1)) {
    ovf++;
  }
  *cnt_out = cnt;
  return ((uint64_t)ovf << RTC_BITS) | cnt;
}

/* Extend a 24 bit captured tick value to the full count, given that it was captured less than 256 s ago. */
static uint64_t extend_locked(uint32_t cap) {
  uint32_t cnt;
  uint64_t now = ticks_now_locked(&cnt);
  int32_t d = (int32_t)((cnt - cap) & RTC_MASK);

  /* The captured value can be one tick ahead of the counter read (clock domain crossing). */
  if (d >= (int32_t)(RTC_MASK >> 1)) {
    d -= (int32_t)(RTC_MASK + 1);
  }
  return now - (int64_t)d;
}

uint64_t ts_tb_ticks_now(void) {
  uint32_t cnt;
  unsigned int key = irq_lock();
  uint64_t t = ticks_now_locked(&cnt);

  irq_unlock(key);
  return t;
}

static uint64_t combine(uint64_t ticks, uint32_t counts) {
  /* Race: a capture within a few PCLK16M cycles after an RTC tick can read the new tick together with the
   * TIMER value of the previous tick (it is cleared by DPPI a little later). Such a value is a full tick count
   * or more: use the tick itself (only with the crystal, see CONFIG_SENSEI_TIMESYNC_HFXO). The error of a capture that
   * really was at the end of a tick is one tick (30.5 us), but that case is rarer than the stale one. */
#ifdef CONFIG_SENSEI_TIMESYNC_HFXO
  if (counts >= COUNTS_GUARD) {
    counts = 0;
  }
#else
  /* TIMER from HFINT (a few % off): counts beyond the tick are not a stale value, limit them to the tick. */
  if (counts > TS_TICK_COUNTS) {
    counts = TS_TICK_COUNTS;
  }
#endif
  return ticks_to_ns(ticks) + counts_to_ns(counts);
}

uint64_t ts_tb_capture_to_ns(uint32_t rtc_cc, uint32_t timer_cc) {
  unsigned int key = irq_lock();
  uint64_t ticks = extend_locked(rtc_cc & RTC_MASK);

  irq_unlock(key);
  return combine(ticks, timer_cc);
}

uint64_t ts_tb_local_ns(void) {
  uint32_t cnt;
  uint32_t cc;
  uint32_t counts;
  uint64_t ticks;
  unsigned int key;

  if (!running) {
    return 0;
  }

  key = irq_lock();
  cnt = nrf_rtc_counter_get(TS_RTC);
  /* A value the capture cannot produce: one before the current counter. */
  const uint32_t stale = (cnt - 1) & RTC_MASK;

  nrf_rtc_cc_set(TS_RTC, TS_CC_NOW, stale);
  nrf_egu_task_trigger(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_NOW));
  cc = nrf_rtc_cc_get(TS_RTC, TS_CC_NOW);
  for (int i = 0; cc == stale && i < CAPTURE_WAIT_LOOPS; i++) {
    cc = nrf_rtc_cc_get(TS_RTC, TS_CC_NOW);
  }
  if (cc == stale) {
    /* Capture did not arrive: use the counter without the sub-tick part. */
    capture_timeouts++;
    ticks = extend_locked(nrf_rtc_counter_get(TS_RTC));
    counts = 0;
  } else {
    counts = nrf_timer_cc_get(TS_TIMER, (nrf_timer_cc_channel_t)TS_CC_NOW);
    ticks = extend_locked(cc);
  }
  irq_unlock(key);
  return combine(ticks, counts);
}

void ts_tb_split(uint64_t local_ns, uint64_t *ticks, uint32_t *counts) {
  uint64_t t = (local_ns << TICK_SHIFT) / TICK_NUM;
  uint32_t r = (uint32_t)(local_ns - ticks_to_ns(t));

#ifdef CONFIG_SENSEI_TIMESYNC_TIMER_16MHZ
  *counts = (r * 2u + 62u) / 125u;
#else
  *counts = (r + 500u) / 1000u;
#endif
  *ticks = t;
}

uint32_t ts_tb_mirror_starts(void) { return mirror_starts; }

bool sensei_ts_local_running(void) { return running; }

void ts_tb_running_cb_set(void (*cb)(void)) { running_cb = cb; }

bool ts_tb_net_started_early(void) { return net_early; }

uint64_t sensei_ts_local_ns(void) { return ts_tb_local_ns(); }

static void rtc_isr(const void *arg) {
  ARG_UNUSED(arg);
  unsigned int key = irq_lock();

  if (nrf_rtc_event_check(TS_RTC, NRF_RTC_EVENT_OVERFLOW)) {
    nrf_rtc_event_clear(TS_RTC, NRF_RTC_EVENT_OVERFLOW);
    rtc_ovf++;
  }
  (void)nrf_rtc_event_check(TS_RTC, NRF_RTC_EVENT_OVERFLOW); /* flush the write */
  irq_unlock(key);
}

static void egu_isr(const void *arg) {
  ARG_UNUSED(arg);
  const nrf_egu_event_t ev_ipc = nrf_egu_triggered_event_get(TS_EGU_CH_IPC);
  const nrf_egu_event_t ev_pps = nrf_egu_triggered_event_get(TS_EGU_CH_PPS);

  if (nrf_egu_event_check(TS_EGU, ev_ipc)) {
    unsigned int key = irq_lock();

    nrf_egu_event_clear(TS_EGU, ev_ipc);
    /* The RTC counter was cleared by hardware: restart the overflow count and drop a stale overflow event. */
    nrf_rtc_event_clear(TS_RTC, NRF_RTC_EVENT_OVERFLOW);
    rtc_ovf = 0;
    mirror_starts++;
    running = true;
    irq_unlock(key);
    if (running_cb) {
      running_cb();
    }
  }
  for (uint8_t k = 0; k < CONFIG_SENSEI_TIMESYNC_CAPTURE_CHANNELS; k++) {
    const nrf_egu_event_t ev = nrf_egu_triggered_event_get(TS_EGU_CH_CAP0 + k);

    if (nrf_egu_event_check(TS_EGU, ev)) {
      nrf_egu_event_clear(TS_EGU, ev);
      ts_capture_isr(k);
    }
  }
  if (nrf_egu_event_check(TS_EGU, ev_pps)) {
    nrf_egu_event_clear(TS_EGU, ev_pps);
    ts_pps_edge_isr();
  }
  (void)nrf_egu_event_check(TS_EGU, ev_pps);
}

#ifdef CONFIG_SENSEI_TIMESYNC_HFXO
static struct onoff_client hfxo_cli;
#endif

static int connect(uint32_t eep, uint32_t tep0, uint32_t tep1, nrfx_gppi_handle_t *h) {
  int err = nrfx_gppi_conn_alloc(eep, tep0, h);

  if (err < 0) {
    return err;
  }
  if (tep1) {
    err = nrfx_gppi_ep_attach(tep1, *h);
  }
  return err < 0 ? err : 0;
}

static int ts_timebase_init(void) {
  nrfx_gppi_handle_t h;
  int err;

#if !defined(CONFIG_TRUSTED_EXECUTION_NONSECURE) && !defined(CONFIG_SOC_SERIES_BSIM_NRFXX)
  /* If the network core is already running, its MPSL may have started the RTC and sent the IPC trigger before
   * the receive side below exists: the mirror would be offset by an unknown time. Not on BabbleSim: both cores start
   * at the same instant and the simulated reset block does not hold the network core. */
  net_early = (NRF_RESET->NETWORK.FORCEOFF == 0);
  if (net_early) {
    LOG_ERR("network core started before the time sync init: mirror clock unreliable");
  }
#endif

  /* RTC: 32768 Hz, runs from boot, cleared by the IPC trigger. */
  nrf_rtc_task_trigger(TS_RTC, NRF_RTC_TASK_STOP);
  nrf_rtc_prescaler_set(TS_RTC, 0);
  nrf_rtc_event_enable(TS_RTC, NRF_RTC_INT_TICK_MASK | NRF_RTC_INT_COMPARE0_MASK);
  nrf_rtc_event_clear(TS_RTC, NRF_RTC_EVENT_OVERFLOW);
  nrf_rtc_int_enable(TS_RTC, NRF_RTC_INT_OVERFLOW_MASK);
  nrf_rtc_task_trigger(TS_RTC, NRF_RTC_TASK_CLEAR);
  IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(TS_RTC), IRQ_PRIO_LOWEST, rtc_isr, NULL, 0);
  irq_enable(NRFX_IRQ_NUMBER_GET(TS_RTC));
  nrf_rtc_task_trigger(TS_RTC, NRF_RTC_TASK_START);

  /* TIMER: 16 MHz or 1 MHz, cleared on every RTC tick. */
  nrf_timer_task_trigger(TS_TIMER, NRF_TIMER_TASK_STOP);
  nrf_timer_mode_set(TS_TIMER, NRF_TIMER_MODE_TIMER);
  nrf_timer_bit_width_set(TS_TIMER, NRF_TIMER_BIT_WIDTH_32);
  nrf_timer_prescaler_set(TS_TIMER, TS_TIMER_PRESCALER);
  nrf_timer_task_trigger(TS_TIMER, NRF_TIMER_TASK_CLEAR);

  err = connect(nrf_rtc_event_address_get(TS_RTC, NRF_RTC_EVENT_TICK),
                nrf_timer_task_address_get(TS_TIMER, NRF_TIMER_TASK_CLEAR), 0, &h);
  if (err) {
    LOG_ERR("DPPI: RTC tick to TIMER clear: %d", err);
    return err;
  }
  nrfx_gppi_conn_enable(h);
  nrf_timer_task_trigger(TS_TIMER, NRF_TIMER_TASK_START);

  /* Software capture: EGU trigger -> RTC CC1 and TIMER CC1 in the same cycle. */
  err = connect(nrf_egu_event_address_get(TS_EGU, nrf_egu_triggered_event_get(TS_EGU_CH_NOW)),
                nrf_rtc_task_address_get(TS_RTC, nrf_rtc_capture_task_get(TS_CC_NOW)),
                nrf_timer_task_address_get(TS_TIMER, nrf_timer_capture_task_get(TS_CC_NOW)), &h);
  if (err) {
    LOG_ERR("DPPI: software capture: %d", err);
    return err;
  }
  nrfx_gppi_conn_enable(h);

  /* EGU interrupts: IPC trigger seen, PPS edge done. */
  nrf_egu_int_enable(TS_EGU, nrf_egu_channel_int_get(TS_EGU_CH_IPC) | nrf_egu_channel_int_get(TS_EGU_CH_PPS));
  for (uint8_t k = 0; k < CONFIG_SENSEI_TIMESYNC_CAPTURE_CHANNELS; k++) {
    nrf_egu_int_enable(TS_EGU, nrf_egu_channel_int_get(TS_EGU_CH_CAP0 + k));
  }
  IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(TS_EGU), IRQ_PRIO_LOWEST, egu_isr, NULL, 0);
  irq_enable(NRFX_IRQ_NUMBER_GET(TS_EGU));

  /* IPC trigger from the network core: clear the RTC and tell the EGU interrupt. The mbox driver loads all IPC
   * receive configurations in its init (POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY) and would overwrite this one,
   * so this init runs after it. */
  nrf_ipc_receive_config_set(NRF_IPC, CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL, BIT(CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL));
  err = connect(
      nrf_ipc_event_address_get(
          NRF_IPC, (nrf_ipc_event_t)(NRF_IPC_EVENT_RECEIVE_0 + CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL * sizeof(uint32_t))),
      nrf_rtc_task_address_get(TS_RTC, NRF_RTC_TASK_CLEAR),
      nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_IPC)), &h);
  if (err) {
    LOG_ERR("DPPI: IPC trigger: %d", err);
    return err;
  }
  nrfx_gppi_conn_enable(h);

#ifdef CONFIG_SENSEI_TIMESYNC_HFXO
  sys_notify_init_spinwait(&hfxo_cli.notify);
  err = onoff_request(z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF), &hfxo_cli);
  if (err < 0) {
    LOG_WRN("HFXO request: %d", err);
  }
#endif
  return 0;
}

/* After the mbox driver (it resets the IPC receive configuration) and before the network core is released
 * (soc_late_init_hook() runs after all POST_KERNEL init functions). */
#ifdef CONFIG_MBOX_INIT_PRIORITY
#define TS_INIT_PRIO UTIL_INC(CONFIG_MBOX_INIT_PRIORITY)
#else
#define TS_INIT_PRIO CONFIG_KERNEL_INIT_PRIORITY_DEFAULT
#endif
SYS_INIT(ts_timebase_init, POST_KERNEL, TS_INIT_PRIO);
