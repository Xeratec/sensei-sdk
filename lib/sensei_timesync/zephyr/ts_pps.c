/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_pps.c
 * @brief Timed output: toggles a pin at global times k * period.
 *
 * Hardware path (per edge, armed by the EGU interrupt of the previous edge):
 *   RTC COMPARE0 --DPPI A--> PPI group enable
 *   (group holds DPPI channel B, disabled while the group is disabled)
 *   TIMER COMPARE0 --DPPI B--> GPIOTE OUT toggle + PPI group disable + EGU trigger (edge done)
 * The RTC match enables B on an RTC tick; the TIMER (cleared on every tick) reaches its compare value 1 to
 * TS_TICK_COUNTS - 1 counts later, between two ticks. B disables itself. A is disabled by the EGU interrupt so
 * that the next match of the 24 bit RTC compare (512 s later) does not fire a spurious edge.
 *
 * Pulse mode (width > 0) uses the same single compare: the rising edge at k * period and the falling edge at
 * k * period + width are armed alternately, the falling one by the interrupt of the rising one. The RTC compare
 * needs the target tick at least two ticks ahead, so the width has a lower limit (PULSE_WIDTH_MIN_NS).
 *
 * Watchdog: a delayable work item fires if no edge happened WATCHDOG_MARGIN_MS after the time the armed edge was
 * due (or after an arming failure) and arms again from the newest model.
 */

#include <errno.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>

#include "ts_internal.h"

LOG_MODULE_REGISTER(ts_pps, CONFIG_SENSEI_TIMESYNC_LOG_LEVEL);

/* An edge must be armed at least this far ahead (interrupt and arming latency, RTC compare needs 2 ticks). */
#define ARM_MARGIN_NS 300000ull
/* RTC compare is a 24 bit match: the arming distance must stay well below 512 s. */
#define PERIOD_MAX_NS (256ull * 1000000000ull)
/* Shortest pulse: the falling edge is armed by the interrupt of the rising edge (latency plus two RTC ticks). */
#define PULSE_WIDTH_MIN_NS 100000ull
/* The watchdog waits this long after the due time of the armed edge. */
#define WATCHDOG_MARGIN_MS 100

static struct {
  bool run;
  bool armed;
  uint64_t period;
  uint64_t width; /* 0: toggle mode (50 % duty); else pulse mode */
  uint64_t armed_k;
  bool armed_fall; /* the armed edge is a falling edge (pulse mode) */
  uint64_t next_k;
  bool next_fall;    /* the next edge is the falling edge of next_k (pulse mode) */
  uint32_t wd_count; /* watchdog re-arms */
  struct k_work_delayable wd;
  nrfx_gpiote_t *g;
  nrfx_gpiote_pin_t pin;
  uint8_t gch;
  nrfx_gppi_handle_t hA, hB;
  nrfx_gppi_group_handle_t group;
  uint32_t eep_rtc, tep_group_en, eep_timer, tep_gpiote;
} pps;

static void force_level(bool high) { nrfx_gpiote_out_task_force(pps.g, pps.pin, high ? 1 : 0); }

/* Interrupts locked. */
static void disarm(void) {
  nrfx_gppi_conn_disable(pps.hA);
  nrfx_gppi_group_disable(pps.group);
  pps.armed = false;
}

static void wd_schedule(int64_t ms) { k_work_reschedule(&pps.wd, K_MSEC(ms + WATCHDOG_MARGIN_MS)); }

/* Interrupts locked. Arm the next edge from the newest model, or stop with the pin low without a global time. */
static void arm(void) {
  struct sensei_ts_time now;
  uint64_t tl;
  uint64_t k;
  bool fall = false;

  if (!sensei_ts_local_running() || sensei_ts_now(&now) != 0) {
    if (pps.armed) {
      disarm();
    }
    force_level(false);
    k_work_cancel_delayable(&pps.wd);
    return;
  }
  if (pps.width && pps.next_fall) {
    /* The falling edge of the pulse that just started. */
    k = pps.next_k;
    fall = true;
    pps.next_fall = false;
    if (sensei_ts_global_to_local(k * pps.period + pps.width, &tl) != 0) {
      force_level(false);
      return;
    }
    if (tl < ts_tb_local_ns() + 2 * ts_tb_ticks_to_ns(1)) {
      /* Too late to arm it: end the pulse now and continue with the next rising edge. */
      LOG_WRN("PPS: pulse edge cannot be armed in time, pulse cut");
      force_level(false);
      pps.next_k = k + 1;
      fall = false;
    }
  }
  if (!fall) {
    k = now.ns / pps.period + 1;
    if (k < pps.next_k) {
      k = pps.next_k;
    }
    for (int tries = 0;; tries++) {
      uint64_t nl = ts_tb_local_ns();

      if (sensei_ts_global_to_local(k * pps.period, &tl) != 0) {
        force_level(false);
        return;
      }
      if (tl > nl + ARM_MARGIN_NS) {
        break;
      }
      if (tries >= 2) {
        LOG_WRN("PPS: cannot arm in time");
        wd_schedule(pps.period / 1000000ull);
        return;
      }
      k++;
    }
  }

  uint64_t ticks;
  uint32_t counts;

  ts_tb_split(tl, &ticks, &counts);
  /* The compare value must lie between two ticks, at least CONFIG_SENSEI_TIMESYNC_PPS_GUARD_COUNTS before the end of
   * the tick (the TIMER is cleared by the next tick). Out of range values use the nearer of the two ways. */
  const uint32_t hi = TS_TICK_COUNTS - CONFIG_SENSEI_TIMESYNC_PPS_GUARD_COUNTS;

  if (counts < 1) {
    counts = 1;
  } else if (counts > hi) {
    if ((TS_TICK_COUNTS + 1) - counts < counts - hi) {
      ticks++;
      counts = 1;
    } else {
      counts = hi;
    }
  }

  disarm();
  /* The level before the edge: toggle mode: high before the falling edges (odd k); pulse mode: high before the
   * falling edge only. Idempotent in steady state. */
  force_level(pps.width ? fall : (k & 1));
  nrf_timer_cc_set(TS_TIMER, (nrf_timer_cc_channel_t)TS_CC_PPS, counts);
  nrf_rtc_cc_set(TS_RTC, TS_CC_PPS, (uint32_t)ticks & 0xFFFFFFu);
  nrf_egu_event_clear(TS_EGU, nrf_egu_triggered_event_get(TS_EGU_CH_PPS));
  nrfx_gppi_conn_enable(pps.hA);
  pps.armed = true;
  pps.armed_k = k;
  pps.armed_fall = fall;
  {
    uint64_t nl = ts_tb_local_ns();

    wd_schedule(tl > nl ? (int64_t)((tl - nl) / 1000000ull) : 0);
  }
}

static void wd_handler(struct k_work *work) {
  ARG_UNUSED(work);
  unsigned int key = irq_lock();

  if (pps.run && sensei_ts_state_get() != SENSEI_TS_UNSYNCED) {
    pps.wd_count++;
    LOG_WRN("PPS watchdog: no edge, re-arming (count %u)", pps.wd_count);
    if (pps.armed) {
      disarm();
    }
    pps.next_fall = false;
    arm();
  }
  irq_unlock(key);
}

void ts_pps_edge_isr(void) {
  if (!pps.run) {
    return;
  }
  unsigned int key = irq_lock();

  nrfx_gppi_conn_disable(pps.hA);
  if (pps.armed) {
    if (pps.width && !pps.armed_fall) {
      pps.next_k = pps.armed_k;
      pps.next_fall = true;
    } else {
      pps.next_k = pps.armed_k + 1;
      pps.next_fall = false;
    }
  }
  pps.armed = false;
  arm();
  irq_unlock(key);
}

void ts_pps_poke(void) {
  if (!pps.run) {
    return;
  }
  unsigned int key = irq_lock();

  if (!pps.run) {
    irq_unlock(key);
    return;
  }
  if (sensei_ts_state_get() == SENSEI_TS_UNSYNCED) {
    if (pps.armed) {
      disarm();
    }
    force_level(false);
    pps.next_fall = false;
  } else if (!pps.armed) {
    arm();
  }
  irq_unlock(key);
}

static int pps_start(const struct gpio_dt_spec *pin, uint64_t period_ns, uint64_t width_ns) {
  nrfx_gpiote_t *g;
  nrfx_gpiote_pin_t abs_pin;
  uint8_t gch;
  int err;

  if (!pin || period_ns < 1000000ull || period_ns > PERIOD_MAX_NS) {
    return -EINVAL;
  }
  err = ts_gpio_resolve(pin, &g, &abs_pin);
  if (err) {
    return err;
  }
  unsigned int key = irq_lock();

  if (pps.run) {
    irq_unlock(key);
    return -EBUSY;
  }
  pps.run = true; /* reserve */
  irq_unlock(key);

  (void)nrfx_gpiote_pin_uninit(g, abs_pin);
  if (nrfx_gpiote_channel_alloc(g, &gch) != 0) {
    err = -ENODEV;
    goto fail_res;
  }
  const nrfx_gpiote_output_config_t ocfg = NRFX_GPIOTE_DEFAULT_OUTPUT_CONFIG;
  const nrfx_gpiote_task_config_t tcfg = {
      .task_ch = gch, .polarity = NRF_GPIOTE_POLARITY_TOGGLE, .init_val = NRF_GPIOTE_INITIAL_VALUE_LOW};

  if (nrfx_gpiote_output_configure(g, abs_pin, &ocfg, &tcfg) != 0) {
    err = -EINVAL;
    goto fail_ch;
  }
  nrfx_gpiote_out_task_enable(g, abs_pin);

  pps.g = g;
  pps.pin = abs_pin;
  pps.gch = gch;
  pps.period = period_ns;
  pps.width = width_ns;
  pps.next_fall = false;
  pps.armed_fall = false;
  k_work_init_delayable(&pps.wd, wd_handler);
  pps.next_k = 0;
  pps.armed = false;
  pps.eep_rtc = nrf_rtc_event_address_get(TS_RTC, NRF_RTC_EVENT_COMPARE_0);
  pps.eep_timer = nrf_timer_event_address_get(TS_TIMER, nrf_timer_compare_event_get(TS_CC_PPS));
  pps.tep_gpiote = nrfx_gpiote_out_task_address_get(g, abs_pin);

  if (nrfx_gppi_group_alloc(nrfx_gppi_domain_id_get(pps.eep_rtc), &pps.group) < 0) {
    err = -ENODEV;
    goto fail_out;
  }
  pps.tep_group_en = nrfx_gppi_group_task_en_addr(pps.group);
  if (nrfx_gppi_conn_alloc(pps.eep_rtc, pps.tep_group_en, &pps.hA) < 0) {
    err = -ENODEV;
    goto fail_group;
  }
  if (nrfx_gppi_conn_alloc(pps.eep_timer, pps.tep_gpiote, &pps.hB) < 0) {
    err = -ENODEV;
    goto fail_a;
  }
  nrfx_gppi_ep_attach(nrfx_gppi_group_task_dis_addr(pps.group), pps.hB);
  nrfx_gppi_ep_attach(nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_PPS)), pps.hB);
  /* B belongs to the group and stays disabled until the group is enabled by A. */
  if (nrfx_gppi_group_ep_add(pps.group, pps.eep_timer) < 0) {
    err = -ENODEV;
    goto fail_b;
  }
  nrfx_gppi_group_disable(pps.group);
  nrfx_gppi_conn_disable(pps.hB);
  nrf_egu_int_enable(TS_EGU, nrf_egu_channel_int_get(TS_EGU_CH_PPS));
  ts_tb_running_cb_set(ts_pps_poke);
  ts_pps_poke();
  return 0;

fail_b:
  nrfx_gppi_ep_clear(nrfx_gppi_group_task_dis_addr(pps.group));
  nrfx_gppi_ep_clear(nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_PPS)));
  nrfx_gppi_conn_free(pps.eep_timer, pps.tep_gpiote, pps.hB);
fail_a:
  nrfx_gppi_conn_free(pps.eep_rtc, pps.tep_group_en, pps.hA);
fail_group:
  nrfx_gppi_group_free(pps.group);
fail_out:
  (void)nrfx_gpiote_pin_uninit(g, abs_pin);
fail_ch:
  (void)nrfx_gpiote_channel_free(g, gch);
fail_res:
  pps.run = false;
  return err;
}

int sensei_ts_pps_stop(void) {
  unsigned int key = irq_lock();

  if (!pps.run) {
    irq_unlock(key);
    return -EINVAL;
  }
  disarm();
  force_level(false);
  pps.run = false;
  irq_unlock(key);

  struct k_work_sync wd_sync;

  k_work_cancel_delayable_sync(&pps.wd, &wd_sync);
  nrfx_gppi_conn_disable(pps.hB);
  nrfx_gppi_group_ch_remove(pps.group, (uint32_t)nrfx_gppi_ep_channel_get(pps.eep_timer));
  nrfx_gppi_ep_clear(nrfx_gppi_group_task_dis_addr(pps.group));
  nrfx_gppi_ep_clear(nrf_egu_task_address_get(TS_EGU, nrf_egu_trigger_task_get(TS_EGU_CH_PPS)));
  nrfx_gppi_conn_free(pps.eep_timer, pps.tep_gpiote, pps.hB);
  nrfx_gppi_conn_free(pps.eep_rtc, pps.tep_group_en, pps.hA);
  nrfx_gppi_group_free(pps.group);
  nrfx_gpiote_out_task_disable(pps.g, pps.pin);
  (void)nrfx_gpiote_pin_uninit(pps.g, pps.pin);
  (void)nrfx_gpiote_channel_free(pps.g, pps.gch);
  return 0;
}

int sensei_ts_pps_start(const struct gpio_dt_spec *pin, uint64_t period_ns) { return pps_start(pin, period_ns, 0); }

int sensei_ts_pps_start_pulse(const struct gpio_dt_spec *pin, uint64_t period_ns, uint64_t width_ns) {
  if (width_ns < PULSE_WIDTH_MIN_NS || width_ns > period_ns / 2) {
    return -EINVAL;
  }
  return pps_start(pin, period_ns, width_ns);
}
