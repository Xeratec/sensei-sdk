/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_core.c
 * @brief State machine, clock model and time conversions.
 *
 * Locking:
 * - wlock (mutex): writers of the model side (pairer, accumulator, model). Taken by the Bluetooth RX thread
 *   (anchor reports, samples), the system work queue (holdover timeout) and API callers (reset). Never from
 *   interrupts. The heavy work (least squares in double) runs under it.
 * - slock (spinlock): the published fit, state and counters. Held for a few microseconds, so readers in
 *   interrupts (capture callbacks, PPS re-arm) can convert times. Writers copy the model fit here after each change.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "sensei_timesync/ts_model.h"
#include "sensei_timesync/ts_pair.h"
#include "ts_internal.h"

LOG_MODULE_REGISTER(ts_core, CONFIG_SENSEI_TIMESYNC_LOG_LEVEL);

#define MASTER_ENABLED IS_ENABLED(CONFIG_SENSEI_TIMESYNC_MASTER)
#define PERIOD_NS ((int64_t)CONFIG_SENSEI_TIMESYNC_PAIR_PERIOD_MS * 1000000)
#define BIAS_NS ((int64_t)CONFIG_SENSEI_TIMESYNC_FOLLOWER_BIAS_NS)
/* Pairs rejected for this long (in local time) are no longer screened, so a step of the master reaches the model
 * and its own reset logic. */
#define SCREEN_GIVEUP_NS (2 * PERIOD_NS)

static struct k_mutex wlock;
static struct k_spinlock slock;

/* Guarded by wlock. */
static struct ts_model model;
static struct ts_pairer pairer;
static uint32_t epoch;
static bool epoch_valid;
static struct {
  bool active;
  int64_t base_local, base_ref;
  int64_t sum_local, sum_ref; /* offsets from base */
  uint32_t n;
  int64_t start_local;
  int64_t rej_since; /* local time of the first pair in the current rejection run, -1: none */
} acc;
static uint32_t mirror_seen;
static bool mirror_seen_valid;
static struct k_work_delayable holdover_work;

/* Guarded by slock. */
static struct ts_fit fit_pub;
static enum sensei_ts_state state = MASTER_ENABLED ? SENSEI_TS_MASTER : SENSEI_TS_UNSYNCED;
static sensei_ts_state_cb_t state_cb;
static struct sensei_ts_status stats;
static uint32_t model_resets;

static void set_state(enum sensei_ts_state s) {
  sensei_ts_state_cb_t cb;
  bool changed = false;

  K_SPINLOCK(&slock) {
    if (state != s) {
      state = s;
      changed = true;
    }
    cb = state_cb;
  }
  if (changed) {
    LOG_INF("state %d", s);
    if (cb) {
      cb(s);
    }
    ts_pps_poke();
  }
}

/* The state without a follower model: master if the role is enabled. */
static enum sensei_ts_state base_state(void) { return MASTER_ENABLED ? SENSEI_TS_MASTER : SENSEI_TS_UNSYNCED; }

/* wlock held. Copy the model fit to the published fit and derive the state from it. */
static void publish(bool sample_seen) {
  struct ts_fit f;
  enum sensei_ts_state cur;

  ts_model_fit_get(&model, &f);
  K_SPINLOCK(&slock) {
    fit_pub = f;
    model_resets = model.resets;
    cur = state;
  }
  if (!f.valid) {
    set_state(base_state());
  } else if (sample_seen && cur != SENSEI_TS_SYNCED) {
    set_state(SENSEI_TS_SYNCED);
  }
}

static void acc_clear(void) {
  acc.active = false;
  acc.n = 0;
  acc.rej_since = -1;
}

/* wlock held. Feed the average of the accumulated pairs to the model. */
static void acc_flush(void) {
  if (!acc.active || acc.n == 0) {
    acc_clear();
    return;
  }
  /* Mean of the offsets, rounded to nearest. */
  const int64_t n = acc.n;
  int64_t ml = acc.sum_local >= 0 ? (acc.sum_local + n / 2) / n : -((-acc.sum_local + n / 2) / n);
  int64_t mr = acc.sum_ref >= 0 ? (acc.sum_ref + n / 2) / n : -((-acc.sum_ref + n / 2) / n);
  enum ts_add_result r = ts_model_add(&model, acc.base_local + ml, acc.base_ref + mr);
  bool accepted = false;

  if (r == TS_ADD_INVALID) {
    /* Local time went backwards (mirror restart or any other jump): start the model over with this point. */
    LOG_WRN("local time not after the newest point: model reset");
    ts_model_reset(&model);
    r = ts_model_add(&model, acc.base_local + ml, acc.base_ref + mr);
  }

  K_SPINLOCK(&slock) {
    stats.points_added++;
    if (r == TS_ADD_OUTLIER) {
      stats.outliers++;
    }
  }
  switch (r) {
  case TS_ADD_ACCEPTED:
    accepted = true;
    break;
  case TS_ADD_RESET:
    LOG_WRN("model reset");
    break;
  default:
    break;
  }
  acc_clear();
  publish(accepted);
}

/* wlock held. A restart of the mirror clock (network core restart) makes all stored local times invalid. */
static void check_mirror(void) {
  const uint32_t m = ts_tb_mirror_starts();

  if (m != mirror_seen) {
    if (mirror_seen_valid) {
      LOG_WRN("local clock restarted: model reset");
      ts_model_reset(&model);
      ts_pair_invalidate(&pairer);
      acc_clear();
      publish(false);
    }
    mirror_seen = m;
    mirror_seen_valid = true;
  }
}

void ts_core_follower_pair(int64_t local_ns, int64_t ref_ns) {
  if (acc.active && local_ns - acc.start_local >= PERIOD_NS) {
    acc_flush();
  }

  /* Screen against the prediction of the valid model. */
  struct ts_fit f;

  K_SPINLOCK(&slock) { f = fit_pub; }
  if (f.valid) {
    int64_t pred;
    uint32_t unc;

    if (ts_fit_to_ref(&f, local_ns, &pred, &unc) == 0) {
      int64_t err = ref_ns - pred;
      int64_t lim = (int64_t)CONFIG_SENSEI_TIMESYNC_OUTLIER_NS + unc;

      if (err > lim || err < -lim) {
        if (acc.rej_since < 0) {
          acc.rej_since = local_ns;
        }
        if (local_ns - acc.rej_since < SCREEN_GIVEUP_NS) {
          K_SPINLOCK(&slock) { stats.outliers++; }
          return;
        }
      } else {
        acc.rej_since = -1;
      }
    }
  }

  if (!acc.active) {
    acc.active = true;
    acc.n = 0;
    acc.base_local = local_ns;
    acc.base_ref = ref_ns;
    acc.sum_local = 0;
    acc.sum_ref = 0;
    acc.start_local = local_ns;
  }
  acc.sum_local += local_ns - acc.base_local;
  acc.sum_ref += ref_ns - acc.base_ref;
  acc.n++;
}

void ts_core_follower_anchor(uint16_t counter, uint64_t local_us) {
  struct ts_point p;

  k_mutex_lock(&wlock, K_FOREVER);
  check_mirror();
  if (ts_pair_on_local(&pairer, counter, local_us, &p) == 1) {
    ts_core_follower_pair(p.local_ns, p.ref_ns);
  }
  k_mutex_unlock(&wlock);
}

void ts_core_follower_sample(const struct ts_msg *msg) {
  struct ts_point pts[TS_PAIR_LOCAL_ANCHORS];

  k_mutex_lock(&wlock, K_FOREVER);
  check_mirror();
  if (!epoch_valid || epoch != msg->epoch) {
    if (epoch_valid) {
      LOG_WRN("new master epoch");
    }
    /* The pairer drops its stored reports itself when the epoch of the sample changes. */
    ts_model_reset(&model);
    acc_clear();
    epoch = msg->epoch;
    epoch_valid = true;
    publish(false);
  }
  K_SPINLOCK(&slock) {
    stats.samples_rx++;
    stats.master_epoch = msg->epoch;
  }
  /* Projection span: 2 s of connection events, clamped to 4..32767 (7.5 ms gives 266, 4 s gives 4). A longer
   * span would cross connection updates more easily and let master anchor-clock error grow with distance. */
  if (msg->interval_us > 0) {
    uint32_t span = 2000000u / msg->interval_us;

    pairer.max_span = (uint16_t)CLAMP(span, 4u, 32767u);
  }
  int n = ts_pair_on_master(&pairer, msg, pts, TS_PAIR_LOCAL_ANCHORS);

  for (int i = 0; i < n; i++) {
    ts_core_follower_pair(pts[i].local_ns, pts[i].ref_ns);
  }
  k_mutex_unlock(&wlock);

  /* Sample seen: restart the holdover timeout. The state returns to SYNCED when a point is accepted. */
  k_work_reschedule(&holdover_work, K_MSEC(CONFIG_SENSEI_TIMESYNC_HOLDOVER_MS));
}

void ts_core_follower_param_update(void) {
  k_mutex_lock(&wlock, K_FOREVER);
  ts_pair_invalidate(&pairer);
  acc_flush();
  k_mutex_unlock(&wlock);
}

static void to_holdover(void) {
  enum sensei_ts_state cur;
  bool valid;

  K_SPINLOCK(&slock) {
    cur = state;
    valid = fit_pub.valid;
  }
  if (valid && (cur == SENSEI_TS_SYNCED)) {
    set_state(SENSEI_TS_HOLDOVER);
  }
}

void ts_core_follower_link_lost(void) {
  k_mutex_lock(&wlock, K_FOREVER);
  ts_pair_invalidate(&pairer);
  acc_flush();
  k_mutex_unlock(&wlock);
  k_work_cancel_delayable(&holdover_work);
  to_holdover();
}

static void holdover_handler(struct k_work *w) {
  ARG_UNUSED(w);
  to_holdover();
}

void sensei_ts_follower_reset(void) {
  k_mutex_lock(&wlock, K_FOREVER);
  ts_model_reset(&model);
  ts_pair_invalidate(&pairer);
  acc_clear();
  epoch_valid = false;
  K_SPINLOCK(&slock) { stats.master_epoch = 0; }
  publish(false);
  k_mutex_unlock(&wlock);
  k_work_cancel_delayable(&holdover_work);
  set_state(base_state());
}

/* ---- Public API ---- */

static int to_global(uint64_t local_ns, struct sensei_ts_time *out) {
  int ret = 0;

  K_SPINLOCK(&slock) {
    if (state == SENSEI_TS_MASTER && !fit_pub.valid) {
      out->ns = local_ns;
      out->uncert_ns = 0;
      out->state = SENSEI_TS_MASTER;
    } else if (fit_pub.valid && (state == SENSEI_TS_SYNCED || state == SENSEI_TS_HOLDOVER)) {
      int64_t ref;
      uint32_t unc;

      if (ts_fit_to_ref(&fit_pub, (int64_t)local_ns, &ref, &unc) == 0) {
        out->ns = (uint64_t)(ref + BIAS_NS);
        out->uncert_ns = unc;
        out->state = state;
      } else {
        ret = -EAGAIN;
      }
    } else {
      ret = -EAGAIN;
    }
  }
  return ret;
}

int sensei_ts_local_to_global(uint64_t local_ns, struct sensei_ts_time *out) {
  if (!out) {
    return -EINVAL;
  }
  return to_global(local_ns, out);
}

int sensei_ts_global_to_local(uint64_t global_ns, uint64_t *local_ns) {
  int ret = 0;

  if (!local_ns) {
    return -EINVAL;
  }
  K_SPINLOCK(&slock) {
    if (state == SENSEI_TS_MASTER && !fit_pub.valid) {
      *local_ns = global_ns;
    } else if (fit_pub.valid && (state == SENSEI_TS_SYNCED || state == SENSEI_TS_HOLDOVER)) {
      /* Inverse of ref = y0 + d + llround(d * rate): first guess, then one correction through the forward map. */
      const int64_t ref = (int64_t)global_ns - BIAS_NS;
      double d = (double)(ref - fit_pub.y0_ns) / (1.0 + fit_pub.rate);
      int64_t loc = fit_pub.x0_ns + llround(d);
      int64_t back;

      if (ts_fit_to_ref(&fit_pub, loc, &back, NULL) == 0) {
        loc += ref - back;
        *local_ns = (uint64_t)loc;
      } else {
        ret = -EAGAIN;
      }
    } else {
      ret = -EAGAIN;
    }
  }
  return ret;
}

int sensei_ts_now(struct sensei_ts_time *out) {
  if (!out) {
    return -EINVAL;
  }
  if (!sensei_ts_local_running()) {
    return -EAGAIN;
  }
  return to_global(ts_tb_local_ns(), out);
}

enum sensei_ts_state sensei_ts_state_get(void) {
  enum sensei_ts_state s;

  K_SPINLOCK(&slock) { s = state; }
  return s;
}

void sensei_ts_status_get(struct sensei_ts_status *st) {
  if (!st) {
    return;
  }
  K_SPINLOCK(&slock) {
    *st = stats;
    st->state = state;
    st->points = fit_pub.valid ? fit_pub.points : 0;
    st->rate_ppm = fit_pub.valid ? fit_pub.rate * 1e6 : 0.0;
    st->resid_ns = fit_pub.valid ? fit_pub.resid_ns : 0;
    st->last_point_local_ns = fit_pub.valid ? (uint64_t)fit_pub.last_local_ns : 0;
    st->resets = model_resets;
  }
}

void sensei_ts_state_cb_set(sensei_ts_state_cb_t cb) {
  K_SPINLOCK(&slock) { state_cb = cb; }
}

void sensei_ts_fit_get(struct ts_fit *fit) {
  if (!fit) {
    return;
  }
  K_SPINLOCK(&slock) { *fit = fit_pub; }
}

static int ts_core_init(void) {
  struct ts_model_cfg cfg;

  k_mutex_init(&wlock);
  k_work_init_delayable(&holdover_work, holdover_handler);
  ts_model_cfg_default(&cfg);
  cfg.window = CONFIG_SENSEI_TIMESYNC_WINDOW;
  cfg.min_points = CONFIG_SENSEI_TIMESYNC_MIN_POINTS;
  cfg.outlier_ns = CONFIG_SENSEI_TIMESYNC_OUTLIER_NS;
  ts_model_init(&model, &cfg);
  ts_pair_init(&pairer, 64); /* until the first sample gives the interval, see ts_core_follower_sample */
  acc_clear();
  return 0;
}

SYS_INIT(ts_core_init, POST_KERNEL, 90);
