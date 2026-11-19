/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Clock model: fit, outliers, resets, uncertainty, inverse, bridge. */

#include <errno.h>
#include <string.h>

#include "sensei_timesync/ts_model.h"
#include "test_util.h"

#define S 1000000000ll /* ns per second */

/* ref = local + off0 + rate * (local - t0), rounded to ns. */
static int64_t line(int64_t local, int64_t t0, int64_t off0, double rate) {
  double d = (double)(local - t0) * rate;
  return local + off0 + (int64_t)(d < 0 ? d - 0.5 : d + 0.5);
}

static void test_cfg(void) {
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  CHECK(c.window == 16 && c.min_points == 2 && c.outlier_ns == 50000 && c.reset_after == 3);
  CHECK(c.max_rate_ppb == 500000 && c.holdover_ppb == 1000);

  struct ts_model m;
  ts_model_init(&m, NULL);
  CHECK(m.cfg.window == 16 && !ts_model_valid(&m) && m.resets == 0);

  c.window = 200;
  c.min_points = 0;
  c.reset_after = 0;
  ts_model_init(&m, &c);
  CHECK(m.cfg.window == TS_MODEL_MAX_POINTS && m.cfg.min_points == 1 && m.cfg.reset_after == 1);
  c.window = 1;
  c.min_points = 9;
  ts_model_init(&m, &c);
  CHECK(m.cfg.window == 2 && m.cfg.min_points == 2);
}

static void test_exact_rate0(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  int64_t ref, off = 123456789012ll;
  uint32_t u;
  CHECK(ts_model_to_ref(&m, 5, &ref, &u) == -EAGAIN);
  CHECK(ts_model_to_local(&m, 5, &ref) == -EAGAIN);
  for (int i = 0; i < 20; i++) {
    int64_t t = 1000000000000ll + i * S;
    CHECK(ts_model_add(&m, t, t + off) == TS_ADD_ACCEPTED);
  }
  CHECK(ts_model_valid(&m) && m.fit.rate == 0 && m.fit.resid_ns == 0 && m.fit.points == 16);
  for (int64_t t = -5 * S; t < 3000 * S; t += 77777777777ll + 13) { /* interpolation and extrapolation, both sides */
    CHECK(ts_model_to_ref(&m, t, &ref, NULL) == 0 && ref == t + off);
    int64_t back;
    CHECK(ts_model_to_local(&m, ref, &back) == 0 && back == t);
  }
}

static void test_exact_line(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  const double rate = 12.5e-6;
  const int64_t t0 = 4000 * S, off = -987654321ll;
  for (int i = 0; i < 30; i++) {
    int64_t t = t0 + (int64_t)i * S + (i % 3) * 1000; /* uneven spacing */
    CHECK(ts_model_add(&m, t, line(t, t0, off, rate)) == TS_ADD_ACCEPTED);
  }
  CHECK(fabs(m.fit.rate - rate) < 1e-12);
  CHECK(m.fit.resid_ns <= 1); /* 0.5 ns rounding of the inputs */
  int64_t worst = 0;
  for (int64_t t = t0 - 50 * S; t < t0 + 500 * S; t += 3333333333ll) {
    int64_t ref;
    ts_model_to_ref(&m, t, &ref, NULL);
    int64_t e = i64abs(ref - line(t, t0, off, rate));
    if (e > worst) {
      worst = e;
    }
  }
  CHECK(worst <= 2); /* input rounding of 0.5 ns, extrapolated over 500 s with a 1e-12 rate error */

  /* two points are enough for an exact line */
  struct ts_model m2;
  ts_model_init(&m2, NULL);
  ts_model_add(&m2, 0, 5000);
  CHECK(!ts_model_valid(&m2));
  ts_model_add(&m2, S, S + 5000 + 40000);
  int64_t ref;
  CHECK(ts_model_valid(&m2) && ts_model_to_ref(&m2, 2 * S, &ref, NULL) == 0 && ref == 2 * S + 5000 + 80000);
}

static void test_noise_rate(void) {
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.window = 32;
  struct ts_model m;
  ts_model_init(&m, &c);
  const double rate = 40e-6;
  double sum_err = 0;
  int n = 0;
  for (int i = 0; i < 400; i++) {
    int64_t t = 100 * S + (int64_t)i * S;
    int64_t noise = (int64_t)(rng_gauss(1000.0));
    CHECK(ts_model_add(&m, t, line(t, 100 * S, 7 * S, rate) + noise) != TS_ADD_INVALID);
    if (i >= 40) {
      sum_err += fabs(m.fit.rate - rate);
      n++;
      /* the reported sigma is about the real rate error (allow a factor 6) */
      CHECK(fabs(m.fit.rate - rate) * 1e9 < 6 * (double)m.fit.rate_sigma_ppb + 1);
    }
  }
  /* 1 us noise, 32 points 1 s apart: the rate sigma is 1000 ns / sqrt(2728 s^2) = 19 ppb */
  CHECK(m.fit.rate_sigma_ppb > 10 && m.fit.rate_sigma_ppb < 35);
  CHECK(sum_err / n * 1e9 < 40);
  CHECK(m.fit.resid_ns > 700 && m.fit.resid_ns < 1300);
}

static void test_outlier(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  const double rate = 20e-6;
  for (int i = 0; i < 10; i++) {
    int64_t t = i * S;
    ts_model_add(&m, t, line(t, 0, 0, rate));
  }
  struct ts_fit before;
  ts_model_fit_get(&m, &before);
  /* 1 ms off */
  CHECK(ts_model_add(&m, 10 * S, line(10 * S, 0, 0, rate) + 1000000) == TS_ADD_OUTLIER);
  CHECK(m.outliers_in_row == 1);
  CHECK(memcmp(&before, &m.fit, sizeof(before)) == 0 && m.fit.points == 10);
  /* the outlier did not become the newest point: the next point 1 s later is accepted */
  CHECK(ts_model_add(&m, 11 * S, line(11 * S, 0, 0, rate)) == TS_ADD_ACCEPTED);
  CHECK(m.outliers_in_row == 0 && m.fit.points == 11 && m.resets == 0);
  /* below the threshold is kept */
  CHECK(ts_model_add(&m, 12 * S, line(12 * S, 0, 0, rate) + 30000) == TS_ADD_ACCEPTED);
  /* two outliers, a good point, two outliers: no reset (the count restarts) */
  for (int k = 0; k < 2; k++) {
    CHECK(ts_model_add(&m, (13 + k) * S, line((13 + k) * S, 0, 0, rate) - 900000) == TS_ADD_OUTLIER);
  }
  CHECK(ts_model_add(&m, 15 * S, line(15 * S, 0, 0, rate)) == TS_ADD_ACCEPTED);
  for (int k = 0; k < 2; k++) {
    CHECK(ts_model_add(&m, (16 + k) * S, line((16 + k) * S, 0, 0, rate) - 900000) == TS_ADD_OUTLIER);
  }
  CHECK(m.resets == 0);

  /* check disabled */
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.outlier_ns = 0;
  ts_model_init(&m, &c);
  ts_model_add(&m, 0, 0);
  ts_model_add(&m, S, S);
  ts_model_add(&m, 2 * S, 2 * S);
  CHECK(ts_model_add(&m, 3 * S, 3 * S + 10000) == TS_ADD_ACCEPTED);

  /* a long gap is not mistaken for a step: the threshold grows with the extrapolation uncertainty */
  ts_model_init(&m, NULL);
  for (int i = 0; i < 16; i++) {
    int64_t t = i * S;
    ts_model_add(&m, t, line(t, 0, 0, rate) + (i % 2 ? 800 : -800));
  }
  CHECK(ts_model_add(&m, 600 * S, line(600 * S, 0, 0, rate) + 20000) == TS_ADD_ACCEPTED);
}

static void test_step_reset(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  const double rate = -15e-6;
  for (int i = 0; i < 12; i++) {
    ts_model_add(&m, i * S, line(i * S, 0, 0, rate));
  }
  const int64_t step = 3 * S; /* the master rebooted: its clock is elsewhere */
  CHECK(ts_model_add(&m, 12 * S, line(12 * S, 0, 0, rate) + step) == TS_ADD_OUTLIER);
  CHECK(ts_model_add(&m, 13 * S, line(13 * S, 0, 0, rate) + step) == TS_ADD_OUTLIER);
  CHECK(m.resets == 0);
  CHECK(ts_model_add(&m, 14 * S, line(14 * S, 0, 0, rate) + step) == TS_ADD_RESET);
  CHECK(m.resets == 1 && m.fit.points == 1 && m.count == 1 && m.outliers_in_row == 0);
  CHECK(m.fit.x0_ns == 14 * S && m.fit.y0_ns == line(14 * S, 0, 0, rate) + step);
  CHECK(!ts_model_valid(&m)); /* min_points 2 */
  int64_t ref;
  CHECK(ts_model_to_ref(&m, 15 * S, &ref, NULL) == -EAGAIN);
  /* it follows the new line */
  CHECK(ts_model_add(&m, 15 * S, line(15 * S, 0, 0, rate) + step) == TS_ADD_ACCEPTED);
  CHECK(ts_model_valid(&m) && fabs(m.fit.rate - rate) < 1e-9);
  CHECK(ts_model_to_ref(&m, 16 * S, &ref, NULL) == 0 && i64abs(ref - (line(16 * S, 0, 0, rate) + step)) <= 1);

  /* reset_after 1 resets on the first outlier */
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.reset_after = 1;
  ts_model_init(&m, &c);
  ts_model_add(&m, 0, 0);
  ts_model_add(&m, S, S);
  CHECK(ts_model_add(&m, 2 * S, 2 * S + 5 * S) == TS_ADD_RESET);
}

static void test_max_rate_reset(void) {
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.outlier_ns = 0; /* only the rate check acts */
  struct ts_model m;
  ts_model_init(&m, &c);
  ts_model_add(&m, 0, 0);
  ts_model_add(&m, S, S + 100000); /* 100 ppm: plausible */
  CHECK(m.count == 2 && fabs(m.fit.rate - 100e-6) < 1e-12);
  /* two points 1 ms apart in ref over 1 s: 1000 ppm of rate */
  struct ts_model m2;
  ts_model_init(&m2, &c);
  ts_model_add(&m2, 0, 0);
  CHECK(ts_model_add(&m2, S, S + 1000000) == TS_ADD_RESET);
  CHECK(m2.count == 1 && m2.resets == 1 && m2.fit.x0_ns == S && m2.fit.y0_ns == S + 1000000);
  CHECK(ts_model_add(&m2, 2 * S, 2 * S + 1000000) == TS_ADD_ACCEPTED && m2.fit.rate == 0);
  /* negative rate too */
  ts_model_init(&m2, &c);
  ts_model_add(&m2, 0, 0);
  CHECK(ts_model_add(&m2, S, S - 600000) == TS_ADD_RESET);
}

static void test_invalid_order(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  ts_model_add(&m, 10 * S, 10 * S);
  ts_model_add(&m, 11 * S, 11 * S);
  struct ts_fit f;
  ts_model_fit_get(&m, &f);
  CHECK(ts_model_add(&m, 11 * S, 11 * S) == TS_ADD_INVALID); /* equal */
  CHECK(ts_model_add(&m, 5 * S, 5 * S) == TS_ADD_INVALID);
  CHECK(ts_model_add(&m, INT64_MIN, 0) == TS_ADD_INVALID);
  CHECK(memcmp(&f, &m.fit, sizeof(f)) == 0 && m.count == 2 && m.resets == 0);
  CHECK(ts_model_add(&m, 11 * S + 1, 11 * S + 1) == TS_ADD_ACCEPTED);
}

static void test_uncertainty(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  const double rate = 30e-6;
  for (int i = 0; i < 16; i++) {
    int64_t t = i * S;
    ts_model_add(&m, t, line(t, 0, 0, rate) + (int64_t)rng_gauss(800));
  }
  int64_t ref;
  uint32_t u0, u1, u10, u60, um10;
  CHECK(ts_model_to_ref(&m, 15 * S, &ref, &u0) == 0);
  CHECK(u0 > 100 && u0 < 1000); /* resid / sqrt(n) at the newest point */
  ts_model_to_ref(&m, 16 * S, &ref, &u1);
  ts_model_to_ref(&m, 25 * S, &ref, &u10);
  ts_model_to_ref(&m, 75 * S, &ref, &u60);
  ts_model_to_ref(&m, -25 * S, &ref, &um10);
  CHECK(u0 < u1 && u1 < u10 && u10 < u60);
  CHECK(um10 > u0); /* grows in both directions (the minimum is at the window center) */
  /* 60 s after the newest point: rate sigma over 67.5 s (from the window center) plus the holdover term of 1 ppm */
  double expect60 = 67.5 * (double)m.fit.rate_sigma_ppb + 60000.0;
  CHECK((double)u60 > expect60 * 0.99 && (double)u60 < expect60 * 1.1);
  /* the holdover term applies only after the newest point */
  uint32_t ua, ub;
  ts_model_to_ref(&m, 15 * S + 30 * S, &ref, &ua);
  ts_model_to_ref(&m, 15 * S - 30 * S, &ref, &ub);
  CHECK(ua > ub + 25000);
  /* NULL uncert */
  CHECK(ts_model_to_ref(&m, 100 * S, &ref, NULL) == 0);

  /* saturation */
  CHECK(ts_model_to_ref(&m, INT64_MAX / 2, &ref, &u60) == 0 && u60 == UINT32_MAX);
}

static void test_one_point(void) {
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.min_points = 1;
  struct ts_model m;
  ts_model_init(&m, &c);
  CHECK(ts_model_add(&m, 100 * S, 100 * S + 777) == TS_ADD_ACCEPTED);
  CHECK(ts_model_valid(&m) && m.fit.points == 1 && m.fit.rate == 0 && m.fit.resid_ns == 0);
  CHECK(m.fit.rate_sigma_ppb == c.max_rate_ppb);
  int64_t ref;
  uint32_t u;
  CHECK(ts_model_to_ref(&m, 100 * S, &ref, &u) == 0 && ref == 100 * S + 777 && u == 0);
  CHECK(ts_model_to_ref(&m, 101 * S, &ref, &u) == 0 && ref == 101 * S + 777);
  /* the uncertainty grows fast: max rate (500 ppm) = 500 us after 1 s, plus holdover */
  CHECK(u >= 500000 && u < 510000);
  int64_t l;
  CHECK(ts_model_to_local(&m, 50 * S + 777, &l) == 0 && l == 50 * S);
  /* a second point gives the rate */
  CHECK(ts_model_add(&m, 101 * S, 101 * S + 777 + 5000) == TS_ADD_ACCEPTED && fabs(m.fit.rate - 5e-6) < 1e-12);
  /* explicit reset */
  ts_model_reset(&m);
  CHECK(!ts_model_valid(&m) && m.count == 0 && m.resets == 1);
}

static void test_large_times(void) {
  const int64_t bases[] = {1000000000000000ll, 40000000000000000ll, 4000000000000000000ll};
  for (unsigned b = 0; b < 3; b++) {
    struct ts_model m;
    ts_model_init(&m, NULL);
    const double rate = 65e-6;
    int64_t t0 = bases[b], off = 3 * S;
    for (int i = 0; i < 16; i++) {
      int64_t t = t0 + (int64_t)i * S;
      CHECK(ts_model_add(&m, t, line(t, t0, off, rate)) == TS_ADD_ACCEPTED);
    }
    CHECK(fabs(m.fit.rate - rate) < 1e-12 && m.fit.resid_ns <= 1);
    int64_t worst = 0;
    for (int64_t t = t0 + 0; t < t0 + 100 * S; t += 2500000001ll) {
      int64_t ref, back;
      ts_model_to_ref(&m, t, &ref, NULL);
      int64_t e = i64abs(ref - line(t, t0, off, rate));
      worst = e > worst ? e : worst;
      CHECK(ts_model_to_local(&m, ref, &back) == 0 && i64abs(back - t) <= 1);
    }
    CHECK(worst <= 2);
  }
}

static void test_round_trip(void) {
  struct ts_model m;
  ts_model_init(&m, NULL);
  for (int i = 0; i < 16; i++) {
    int64_t t = 5000000000000ll + i * S;
    ts_model_add(&m, t, line(t, 5000000000000ll, -12 * S, 450e-6) + (i & 1) * 300);
  }
  int64_t worst = 0;
  for (int64_t t = 5000000000000ll - 1000 * S; t < 5000000000000ll + 100000 * S; t += 12345678901ll) {
    int64_t ref, back;
    ts_model_to_ref(&m, t, &ref, NULL);
    ts_model_to_local(&m, ref, &back);
    worst = i64abs(back - t) > worst ? i64abs(back - t) : worst;
  }
  CHECK(worst <= 1);
}

static void test_window_ring(void) {
  struct ts_model_cfg c;
  ts_model_cfg_default(&c);
  c.window = 5;
  struct ts_model m;
  ts_model_init(&m, &c);
  for (int i = 0; i < 23; i++) {
    CHECK(ts_model_add(&m, i * S, i * S + 10) == TS_ADD_ACCEPTED);
  }
  CHECK(m.fit.points == 5 && m.fit.first_local_ns == 18 * S && m.fit.last_local_ns == 22 * S);
  CHECK(m.fit.x0_ns == 22 * S && m.fit.y0_ns == 22 * S + 10);
  /* only the window counts: after a rate change the old line is forgotten */
  for (int i = 23; i < 28; i++) {
    ts_model_add(&m, i * S, i * S + 10 + (i - 22) * 20000);
  }
  CHECK(fabs(m.fit.rate - 20e-6) < 1e-12 && m.fit.first_local_ns == 23 * S);
}

static void test_bridge(void) {
  /* The true offset: slope 40 ppm up to t = 150 s, then 45 ppm (a temperature change during the gap). */
  const double r1 = 40e-6, r2 = 45e-6;
#define TRUTH(t)                                                                                                       \
  ((t) <= 150 * S ? (t) + (int64_t)((double)(t) * r1)                                                                  \
                  : 150 * S + (int64_t)(150e9 * r1) + ((t) - 150 * S) + (int64_t)((double)((t) - 150 * S) * r2))
  struct ts_model a, b;
  ts_model_init(&a, NULL);
  ts_model_init(&b, NULL);
  for (int i = 0; i < 16; i++) {
    int64_t t = (85 + i) * S;
    ts_model_add(&a, t, TRUTH(t));
  }
  for (int i = 0; i < 16; i++) {
    int64_t t = (200 + i) * S;
    ts_model_add(&b, t, TRUTH(t));
  }
  struct ts_fit fa, fb, inv;
  ts_model_fit_get(&a, &fa);
  ts_model_fit_get(&b, &fb);
  memset(&inv, 0, sizeof(inv));
  int64_t g0 = fa.last_local_ns, g1 = fb.first_local_ns, r, ra, rb;
  CHECK(g0 == 100 * S && g1 == 200 * S);

  /* ends: continuous with the two fits */
  ts_fit_to_ref(&fa, g0, &ra, NULL);
  CHECK(ts_fit_bridge(&fa, &fb, g0, &r) == 0 && r == ra);
  ts_fit_to_ref(&fb, g1, &rb, NULL);
  CHECK(ts_fit_bridge(&fa, &fb, g1, &r) == 0 && r == rb);
  /* outside: the nearer fit */
  ts_fit_to_ref(&fa, 50 * S, &ra, NULL);
  CHECK(ts_fit_bridge(&fa, &fb, 50 * S, &r) == 0 && r == ra);
  ts_fit_to_ref(&fb, 300 * S, &rb, NULL);
  CHECK(ts_fit_bridge(&fa, &fb, 300 * S, &r) == 0 && r == rb);
  /* inside: the linear blend, and no worse than the worse of the two single fits at any point of the gap */
  int64_t worst_bridge = 0, worst_a = 0, worst_b = 0;
  for (int64_t t = g0; t <= g1; t += S) {
    ts_fit_bridge(&fa, &fb, t, &r);
    ts_fit_to_ref(&fa, t, &ra, NULL);
    ts_fit_to_ref(&fb, t, &rb, NULL);
    double w = (double)(t - g0) / (double)(g1 - g0);
    CHECK(i64abs(r - (int64_t)((1 - w) * (double)ra + w * (double)rb)) <= 1);
    int64_t tr = TRUTH(t);
    worst_bridge = i64abs(r - tr) > worst_bridge ? i64abs(r - tr) : worst_bridge;
    worst_a = i64abs(ra - tr) > worst_a ? i64abs(ra - tr) : worst_a;
    worst_b = i64abs(rb - tr) > worst_b ? i64abs(rb - tr) : worst_b;
  }
  /* each single fit is off by up to 250 us at the far end of the gap; the bridge by at most half the rate change
   * integrated over the gap (5 ppm * 50 s * 0.5 = 125 us at the middle, 62 us where the weights split) */
  CHECK(worst_a > 200000 && worst_b > 200000);
  CHECK(worst_bridge < 130000 && worst_bridge < worst_a && worst_bridge < worst_b);

  /* invalid fits */
  CHECK(ts_fit_bridge(&inv, &fb, 120 * S, &r) == 0);
  ts_fit_to_ref(&fb, 120 * S, &rb, NULL);
  CHECK(r == rb);
  CHECK(ts_fit_bridge(&fa, &inv, 120 * S, &r) == 0);
  ts_fit_to_ref(&fa, 120 * S, &ra, NULL);
  CHECK(r == ra);
  CHECK(ts_fit_bridge(&inv, &inv, 120 * S, &r) == -EAGAIN);
  /* overlapping fits (no gap): the newer from its first point on */
  CHECK(ts_fit_bridge(&fb, &fa, 90 * S, &r) == 0);
}

int main(void) {
  test_cfg();
  test_exact_rate0();
  test_exact_line();
  test_noise_rate();
  test_outlier();
  test_step_reset();
  test_max_rate_reset();
  test_invalid_order();
  test_uncertainty();
  test_one_point();
  test_large_times();
  test_round_trip();
  test_window_ring();
  test_bridge();
  printf("ts_model: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
