/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gnss.c
 * @brief One GNSS fix on request, with the nRF modem library (nrf_modem_gnss).
 *
 * A worker thread runs an attempt: it starts the modem library if needed, makes sure GNSS is in the system mode and
 * activates it (functional mode 31, lte_gnss_begin: antenna, system mode), starts a single fix, waits for a valid fix,
 * the timeout or a cancel, stops, deactivates GNSS (lte_gnss_end: functional mode 30, or 0 if the modem was off (0 or
 * 4) when the fix began or LTE was released during it) and queues the result event. GNSS is active only for the
 * attempt. The modem runs GNSS only while LTE is idle; a fix that the modem kept blocked until the end is reported as
 * such. Untested on hardware.
 */

#include <errno.h>
#include <string.h>

#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "gnss.h"
#include "lte.h"
#include "sensei_lpwan/lpwan_gnss.h"
#include "service.h"

LOG_MODULE_REGISTER(gnss, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(event_sem, 0, 1);
static struct k_thread thread;
K_THREAD_STACK_DEFINE(gnss_stack, CONFIG_LPWAN_SHIELD_GNSS_STACK);

static atomic_t running;
static atomic_t cancel_req;
static atomic_t blocked;   /* the modem blocks GNSS now (BLOCKED, no UNBLOCKED since) */
static atomic_t no_window; /* the last PVT said that the time windows were missed or too short */
static atomic_t sleeping;  /* the modem ended the attempt itself (fix retry time reached) */
static uint32_t req_seq, req_timeout_s;

static struct k_spinlock pvt_lock;
static struct nrf_modem_gnss_pvt_data_frame pvt;
static atomic_t pvt_new;
static int64_t pvt_ms; /* uptime when the handler read the PVT frame, under pvt_lock */

/* LpwanStatus counters since the shield started */
static uint32_t n_attempts, n_fixes, on_ms;

/* The modem library calls this from its own context: keep it short. */
static void gnss_handler(int event) {
  switch (event) {
  case NRF_MODEM_GNSS_EVT_PVT:
  case NRF_MODEM_GNSS_EVT_FIX: {
    struct nrf_modem_gnss_pvt_data_frame f;

    if (nrf_modem_gnss_read(&f, sizeof(f), NRF_MODEM_GNSS_DATA_PVT) == 0) {
      k_spinlock_key_t k = k_spin_lock(&pvt_lock);

      pvt = f;
      pvt_ms = k_uptime_get();
      k_spin_unlock(&pvt_lock, k);
      atomic_set(&no_window, (f.flags & (NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED |
                                         NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME)) != 0);
      atomic_set(&pvt_new, 1);
    }
    break;
  }
  case NRF_MODEM_GNSS_EVT_BLOCKED:
    atomic_set(&blocked, 1);
    break;
  case NRF_MODEM_GNSS_EVT_UNBLOCKED:
    atomic_set(&blocked, 0);
    break;
  case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
    atomic_set(&sleeping, 1);
    break;
  default:
    return;
  }
  k_sem_give(&event_sem);
}

static void fill_fix(sensei_GnssFixResult *r, const struct nrf_modem_gnss_pvt_data_frame *f) {
  uint32_t sats = 0;

  for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
    if (f->sv[i].sv != 0 && (f->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX)) {
      sats++;
    }
  }
  r->lat_e7 = lpwan_gnss_deg_e7(f->latitude, 90);
  r->lon_e7 = lpwan_gnss_deg_e7(f->longitude, 180);
  r->alt_m = (int32_t)(f->altitude >= 0 ? f->altitude + 0.5f : f->altitude - 0.5f);
  r->acc_m = lpwan_gnss_acc_m(f->accuracy);
  r->sats = sats;
  r->unix_time = lpwan_gnss_utc_to_unix_ms(f->datetime.year, f->datetime.month, f->datetime.day, f->datetime.hour,
                                           f->datetime.minute, f->datetime.seconds, f->datetime.ms);
}

/* One attempt. Returns the result to report. */
static void run_fix(sensei_GnssFixResult *res) {
  int64_t t_on = 0, t_start, fix_at = 0;
  bool fix = false, error = false, started = false;
  int64_t end;
  int err;

  res->seq = req_seq;
  atomic_set(&blocked, 0);
  atomic_set(&no_window, 0);
  atomic_set(&sleeping, 0);
  atomic_set(&pvt_new, 0);
  n_attempts++;
  t_start = k_uptime_get();
  end = t_start + (int64_t)req_timeout_s * 1000;

  err = lte_gnss_begin();
  if (err) {
    LOG_ERR("GNSS: modem not ready (%d)", err);
    error = true;
    goto done;
  }
  t_on = k_uptime_get();
  err = nrf_modem_gnss_event_handler_set(gnss_handler);
  err |= nrf_modem_gnss_nmea_mask_set(0);    /* no NMEA strings: nobody reads them */
  err |= nrf_modem_gnss_fix_interval_set(0); /* single fix */
  err |= nrf_modem_gnss_fix_retry_set((uint16_t)req_timeout_s);
  if (err == 0) {
    t_start = k_uptime_get();
    err = nrf_modem_gnss_start();
  }
  if (err) {
    LOG_ERR("GNSS: start failed (%d)", err);
    error = true;
    goto done;
  }
  started = true;
  LOG_INF("GNSS: fix attempt, timeout %u s", (unsigned)req_timeout_s);
  while (!fix && !atomic_get(&cancel_req) && !atomic_get(&sleeping)) {
    int64_t left = end - k_uptime_get();

    if (left <= 0) {
      break;
    }
    (void)k_sem_take(&event_sem, K_MSEC(left));
    if (atomic_cas(&pvt_new, 1, 0)) {
      struct nrf_modem_gnss_pvt_data_frame f;
      k_spinlock_key_t k = k_spin_lock(&pvt_lock);
      int64_t f_ms;

      f = pvt;
      f_ms = pvt_ms;
      k_spin_unlock(&pvt_lock, k);
      if (f.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        fill_fix(res, &f);
        res->ttff_ms = (uint32_t)(f_ms - t_start);
        fix_at = f_ms;
        fix = true;
      }
    }
  }
done:
  if (started) {
    (void)nrf_modem_gnss_stop();
  }
  if (t_on != 0) {
    lte_gnss_end();
    on_ms += (uint32_t)(k_uptime_get() - t_on);
  }
  if (fix) {
    res->age_ms = (uint32_t)(k_uptime_get() - fix_at); /* includes the time that lte_gnss_end() took */
  }
  if (!fix) {
    res->ttff_ms = (uint32_t)(k_uptime_get() - t_start);
  }
  res->status = lpwan_gnss_end_status(fix, atomic_get(&cancel_req) != 0, error,
                                      atomic_get(&blocked) != 0 || atomic_get(&no_window) != 0);
  if (fix) {
    n_fixes++;
  }
  LOG_INF("GNSS: %s after %u ms, %u satellites", lpwan_gnss_status_name(res->status), (unsigned)res->ttff_ms,
          (unsigned)res->sats);
}

static void gnss_main(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    static sensei_GnssFixResult res;

    (void)k_sem_take(&start_sem, K_FOREVER);
    memset(&res, 0, sizeof(res));
    run_fix(&res);
    atomic_set(&running, 0); /* before the event: the master may start LTE as soon as it sees it */
    svc_emit_gnss_result(&res);
  }
}

int gnss_init(void) {
  k_thread_create(&thread, gnss_stack, K_THREAD_STACK_SIZEOF(gnss_stack), gnss_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_WORKER_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&thread, "gnss");
  return 0;
}

sensei_LpwanResult gnss_start(uint32_t seq, uint32_t timeout_s) {
  uint32_t t;

  if (!lpwan_gnss_timeout(timeout_s, &t)) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  if (!atomic_cas(&running, 0, 1)) {
    return sensei_LpwanResult_LPWAN_BUSY;
  }
  req_seq = seq;
  req_timeout_s = t;
  atomic_set(&cancel_req, 0);
  k_sem_give(&start_sem);
  return sensei_LpwanResult_LPWAN_OK;
}

sensei_LpwanResult gnss_cancel(void) {
  if (atomic_get(&running)) {
    atomic_set(&cancel_req, 1);
    k_sem_give(&event_sem);
  }
  return sensei_LpwanResult_LPWAN_OK;
}

bool gnss_active(void) { return atomic_get(&running) != 0; }

void gnss_fill_status(sensei_LpwanStatus *st) {
  st->has_gnss_attempts = st->has_gnss_fixes = st->has_gnss_on_ms = true;
  st->gnss_attempts = n_attempts;
  st->gnss_fixes = n_fixes;
  st->gnss_on_ms = on_ms;
}
