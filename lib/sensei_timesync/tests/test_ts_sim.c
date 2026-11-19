/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Two free-running clocks over a BLE connection, through the whole chain: master sample (wire format) -> pairing
 * -> clock model -> conversion. The master clock runs +40 ppm, the follower clock -25 ppm (both against true
 * time). Connection events are 100 ms apart on the master clock. Each reported anchor carries white Gaussian jitter
 * and is quantized to 1 us like the controller report. The model gets one point per second.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sensei_timesync/ts_model.h"
#include "sensei_timesync/ts_msg.h"
#include "sensei_timesync/ts_pair.h"
#include "test_util.h"

#define MASTER_PPM 40e-6
#define FOLLOWER_PPM (-25e-6)
#define INTERVAL_US 100000
#define EVENTS_PER_SAMPLE 10    /* 1 s */
#define MASTER_A0 123456789.0   /* master clock at event 0 (us) */
#define FOLLOWER_L0 987654321.0 /* follower clock at event 0 (us) */
#define FIRST_COUNTER 65500     /* the counter wraps during the run */

struct sim {
  struct ts_model model;
  struct ts_pairer pairer;
  double jitter_us;
  uint32_t epoch;
  int second;
};

/* True master time (ns) at a follower local time (ns), from the clock definitions. */
static double truth_ref_ns(double local_ns) {
  double t_us = (local_ns / 1000.0 - FOLLOWER_L0) / (1.0 + FOLLOWER_PPM); /* true time since event 0 */
  return (MASTER_A0 + t_us * (1.0 + MASTER_PPM)) * 1000.0;
}

static double event_true_us(int k) { return (double)k * INTERVAL_US / (1.0 + MASTER_PPM); }

static uint16_t counter_of(int k) { return (uint16_t)(FIRST_COUNTER + k); }

static double jitter(const struct sim *s) { return s->jitter_us > 0 ? rng_gauss(s->jitter_us) : 0; }

static void sim_init(struct sim *s, const struct ts_model_cfg *cfg, double jitter_us) {
  memset(s, 0, sizeof(*s));
  ts_model_init(&s->model, cfg);
  ts_pair_init(&s->pairer, 100);
  s->jitter_us = jitter_us;
  s->epoch = 0x12345678;
}

/*
 * One second: the master sends the sample of its event k, the follower pairs it with its own report of event k + 5
 * and feeds the model. Returns the result of ts_model_add(), or -1 if no point came out.
 */
static int sim_step(struct sim *s) {
  int k = s->second * EVENTS_PER_SAMPLE;
  s->second++;

  struct ts_msg m = {.counter = counter_of(k), .epoch = s->epoch, .interval_us = INTERVAL_US};
  m.anchor_us = (uint64_t)llround(MASTER_A0 + (double)k * INTERVAL_US + jitter(s));
  uint8_t wire[TS_MSG_LEN];
  struct ts_msg rx;
  if (ts_msg_encode(&m, wire, sizeof(wire)) != TS_MSG_LEN || ts_msg_decode(&rx, wire, sizeof(wire)) != 0) {
    return -2;
  }
  struct ts_point pts[TS_PAIR_LOCAL_ANCHORS];
  ts_pair_on_master(&s->pairer, &rx, pts, TS_PAIR_LOCAL_ANCHORS);

  int ke = k + 5;
  double local = FOLLOWER_L0 + event_true_us(ke) * (1.0 + FOLLOWER_PPM) + jitter(s);
  struct ts_point pt;
  if (ts_pair_on_local(&s->pairer, counter_of(ke), (uint64_t)llround(local), &pt) != 1) {
    return -1;
  }
  return ts_model_add(&s->model, pt.local_ns, pt.ref_ns);
}

struct stats {
  double sum2, max_abs;
  int n;
};

static void stat_add(struct stats *st, double e) {
  st->sum2 += e * e;
  st->n++;
  if (fabs(e) > st->max_abs) {
    st->max_abs = fabs(e);
  }
}

static double stat_rms(const struct stats *st) { return st->n ? sqrt(st->sum2 / st->n) : 0; }

/* Error of the model (ns) at a local time, against the clock definitions; also checks the uncertainty. */
static double model_err(const struct sim *s, double local_ns, uint32_t *u) {
  int64_t l = llround(local_ns), ref;
  if (ts_model_to_ref(&s->model, l, &ref, u) != 0) {
    return 1e18;
  }
  return (double)ref - truth_ref_ns((double)l);
}

static void test_realistic(void) {
  struct sim s;
  sim_init(&s, NULL, 1.0);
  struct stats ext = {0}, newest = {0}, interp = {0};
  int within_1sigma = 0, n_u = 0;
  const int N = 1200;
  for (int i = 0; i < N; i++) {
    int r = sim_step(&s);
    CHECK(r == TS_ADD_ACCEPTED || r == -1 || r == TS_ADD_OUTLIER);
    if (i < 30) {
      continue;
    }
    const struct ts_fit *f = &s.model.fit;
    /* the model must run through the whole run without a reset after its first points */
    double x0 = (double)f->x0_ns;
    /* now: the newest point; extrapolation: up to 1 s after it (events between the updates);
     * interpolation: inside the window */
    uint32_t u;
    double e = model_err(&s, x0, &u);
    stat_add(&newest, e);
    if (fabs(e) <= u) {
      within_1sigma++;
    }
    n_u++;
    for (int j = 1; j <= 9; j++) {
      stat_add(&ext, model_err(&s, x0 + j * 1e8, NULL));
    }
    for (int j = 1; j <= 14; j++) {
      stat_add(&interp, model_err(&s, x0 - j * 1e9 + 5e8, NULL));
    }
  }
  CHECK(s.model.resets == 0);
  const double true_rate = (1.0 + MASTER_PPM) / (1.0 + FOLLOWER_PPM) - 1.0;
  printf("sim: fit rate %.4f ppm (true %.4f), resid %u ns, rate sigma %u ppb\n", s.model.fit.rate * 1e6,
         true_rate * 1e6, s.model.fit.resid_ns, s.model.fit.rate_sigma_ppb);
  printf("sim: error at the newest point:   rms %.1f ns, max %.1f ns\n", stat_rms(&newest), newest.max_abs);
  printf("sim: error 0.1..0.9 s after it:   rms %.1f ns, max %.1f ns\n", stat_rms(&ext), ext.max_abs);
  printf("sim: error inside the window:     rms %.1f ns, max %.1f ns\n", stat_rms(&interp), interp.max_abs);
  printf("sim: |error| <= reported uncertainty at the newest point in %d of %d seconds\n", within_1sigma, n_u);

  /* The fit is exact in rate to well below the noise: 1 us / sqrt(Sxx) */
  CHECK(fabs(s.model.fit.rate - true_rate) < 0.3e-6);
  /* Converted times are better than 1 us (rms) and than 2 us (worst case over 1170 s and all the evaluated times) */
  CHECK(stat_rms(&newest) < 1000 && stat_rms(&ext) < 1000 && stat_rms(&interp) < 1000);
  CHECK(newest.max_abs < 3000 && ext.max_abs < 3000 && interp.max_abs < 3000);
  /* the uncertainty is a one-sigma estimate: it covers the error most of the time */
  CHECK(within_1sigma > n_u * 5 / 10);
}

/* Several noise levels and window sizes: the error scales with the jitter and drops with the window. */
static void test_scaling(void) {
  double prev = 1e18;
  const uint8_t windows[] = {4, 8, 16, 32};
  for (unsigned w = 0; w < 4; w++) {
    struct ts_model_cfg c;
    ts_model_cfg_default(&c);
    c.window = windows[w];
    struct sim s;
    sim_init(&s, &c, 1.0);
    struct stats st = {0};
    for (int i = 0; i < 800; i++) {
      sim_step(&s);
      if (i >= 50) {
        stat_add(&st, model_err(&s, (double)s.model.fit.x0_ns + 5e8, NULL));
      }
    }
    printf("sim: window %2u: rms %.1f ns, max %.1f ns at half a second after the newest point\n", windows[w],
           stat_rms(&st), st.max_abs);
    CHECK(stat_rms(&st) < prev * 1.05);
    prev = stat_rms(&st);
  }
  /* no jitter: only the 1 us quantization of the reports */
  struct sim s;
  sim_init(&s, NULL, 0.0);
  struct stats st = {0};
  for (int i = 0; i < 300; i++) {
    sim_step(&s);
    if (i >= 30) {
      stat_add(&st, model_err(&s, (double)s.model.fit.x0_ns + 5e8, NULL));
    }
  }
  printf("sim: no jitter (1 us quantization only): rms %.1f ns, max %.1f ns\n", stat_rms(&st), st.max_abs);
  CHECK(st.max_abs < 1000);
}

static void test_holdover(void) {
  struct sim s;
  sim_init(&s, NULL, 1.0);
  for (int i = 0; i < 200; i++) {
    sim_step(&s);
  }
  CHECK(s.model.resets == 0);
  struct ts_fit f;
  ts_model_fit_get(&s.model, &f);
  /* the master link stops: no more points, 60 s of holdover (no wander in the simulation) */
  uint32_t prev_u = 0;
  int covered = 0, steps = 0;
  double err60 = 0;
  uint32_t u60 = 0;
  for (int dt = 0; dt <= 60; dt++) {
    double now = (double)f.x0_ns + dt * 1e9;
    uint32_t u;
    double e = model_err(&s, now, &u);
    if (dt > 0) {
      CHECK(u > prev_u);
    }
    prev_u = u;
    if (fabs(e) <= (double)u) {
      covered++;
    }
    steps++;
    if (dt == 60) {
      err60 = e;
      u60 = u;
    }
  }
  printf("sim: holdover 60 s: error %.0f ns, reported uncertainty %u ns\n", err60, u60);
  CHECK(covered == steps);
  CHECK(u60 > 10 * prev_u / 100 && u60 >= 60000); /* at least the 1 ppm holdover wander over 60 s */

  /* the link comes back after 60 s: the first point is no outlier and the model keeps its history */
  s.second += 60;
  int r = sim_step(&s);
  CHECK(r == TS_ADD_ACCEPTED);
  CHECK(s.model.resets == 0 && s.model.fit.points == 16);
  struct stats st = {0};
  for (int i = 0; i < 30; i++) {
    sim_step(&s);
    if (i >= 20) {
      stat_add(&st, model_err(&s, (double)s.model.fit.x0_ns + 5e8, NULL));
    }
  }
  CHECK(st.max_abs < 3000);
}

static void test_master_reboot(void) {
  struct sim s;
  sim_init(&s, NULL, 1.0);
  for (int i = 0; i < 60; i++) {
    sim_step(&s);
  }
  /* the master reboots: new epoch, its clock restarts at another value (here: 1 hour ahead) */
  s.epoch = 0xCAFEF00D;
  int resets = 0;
  struct ts_msg m = {.counter = counter_of(s.second * EVENTS_PER_SAMPLE), .epoch = s.epoch, .interval_us = INTERVAL_US};
  m.anchor_us = (uint64_t)(MASTER_A0 + 3.6e9 + s.second * EVENTS_PER_SAMPLE * (double)INTERVAL_US);
  struct ts_point pts[TS_PAIR_LOCAL_ANCHORS];
  ts_pair_on_master(&s.pairer, &m, pts, TS_PAIR_LOCAL_ANCHORS);
  /* from here the follower sees the new master clock: feed points of the shifted master through the model */
  for (int i = 0; i < 10; i++) {
    int k = (s.second + i) * EVENTS_PER_SAMPLE;
    struct ts_point pt;
    double local = FOLLOWER_L0 + event_true_us(k + 5) * (1.0 + FOLLOWER_PPM);
    int n = ts_pair_on_local(&s.pairer, counter_of(k + 5), (uint64_t)llround(local), &pt);
    if (n != 1) {
      continue;
    }
    if (ts_model_add(&s.model, pt.local_ns, pt.ref_ns) == TS_ADD_RESET) {
      resets++;
    }
  }
  CHECK(resets >= 1 && s.model.resets >= 1);
  /* after the reset the model holds only new-master points */
  CHECK(s.model.fit.points >= 1 && s.model.fit.y0_ns - s.model.fit.x0_ns > 2500000000000ll);
}

int main(void) {
  test_realistic();
  test_scaling();
  test_holdover();
  test_master_reboot();
  printf("ts_sim: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
