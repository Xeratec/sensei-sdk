/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_timesync/ts_model.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/*
 * Fit: the offset o(t) = ref - local is a line in the local time t. The points are centered on the newest point
 * (x = local - local_newest, y = o - o_newest, both exact int64 differences converted to double), so the double
 * precision does not depend on the absolute time (11 days = 1e15 ns would leave only 0.1 ns in a double).
 *
 * With only two points the scatter of the points is unknown: the rate sigma then assumes TS_ASSUMED_JITTER_NS.
 */
#define TS_ASSUMED_JITTER_NS 1000.0

static int64_t round_i64(double x) { return (int64_t)(x < 0 ? x - 0.5 : x + 0.5); }

static uint32_t sat_u32(float x) { return x >= 4294967040.0f ? UINT32_MAX : (uint32_t)(x + 0.5f); }

void ts_model_cfg_default(struct ts_model_cfg *cfg) {
  cfg->window = 16;
  cfg->min_points = 2;
  cfg->outlier_ns = 50000;
  cfg->reset_after = 3;
  cfg->max_rate_ppb = 500000;
  cfg->holdover_ppb = 1000;
}

void ts_model_reset(struct ts_model *m) {
  m->head = 0;
  m->count = 0;
  m->outliers_in_row = 0;
  m->resets++;
  memset(&m->fit, 0, sizeof(m->fit));
  m->fit.holdover_ppb = m->cfg.holdover_ppb;
}

void ts_model_init(struct ts_model *m, const struct ts_model_cfg *cfg) {
  memset(m, 0, sizeof(*m));
  if (cfg != NULL) {
    m->cfg = *cfg;
  } else {
    ts_model_cfg_default(&m->cfg);
  }
  if (m->cfg.window < 2) {
    m->cfg.window = 2;
  }
  if (m->cfg.window > TS_MODEL_MAX_POINTS) {
    m->cfg.window = TS_MODEL_MAX_POINTS;
  }
  if (m->cfg.min_points < 1) {
    m->cfg.min_points = 1;
  }
  if (m->cfg.min_points > m->cfg.window) {
    m->cfg.min_points = m->cfg.window;
  }
  if (m->cfg.reset_after < 1) {
    m->cfg.reset_after = 1;
  }
  m->fit.holdover_ppb = m->cfg.holdover_ppb;
}

bool ts_model_valid(const struct ts_model *m) { return m->count >= m->cfg.min_points; }

/* Index of the i-th stored point, oldest first. */
static uint8_t slot(const struct ts_model *m, uint8_t i) {
  return (uint8_t)((m->head + m->cfg.window - m->count + i) % m->cfg.window);
}

static void push(struct ts_model *m, int64_t local_ns, int64_t ref_ns) {
  m->local_ns[m->head] = local_ns;
  m->ref_ns[m->head] = ref_ns;
  m->head = (uint8_t)((m->head + 1) % m->cfg.window);
  if (m->count < m->cfg.window) {
    m->count++;
  }
}

static void recompute(struct ts_model *m) {
  struct ts_fit *f = &m->fit;
  uint8_t n = m->count;
  uint8_t first = slot(m, 0), last = slot(m, (uint8_t)(n - 1));
  int64_t xn = m->local_ns[last], yn = m->ref_ns[last] - m->local_ns[last];

  f->valid = n >= m->cfg.min_points;
  f->points = n;
  f->x0_ns = xn;
  f->y0_ns = m->ref_ns[last];
  f->rate = 0;
  f->resid_ns = 0;
  f->rate_sigma_ppb = m->cfg.max_rate_ppb;
  f->first_local_ns = m->local_ns[first];
  f->last_local_ns = xn;
  f->holdover_ppb = m->cfg.holdover_ppb;
  if (n < 2) {
    return;
  }

  double sx = 0, sy = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t k = slot(m, i);
    sx += (double)(m->local_ns[k] - xn);
    sy += (double)((m->ref_ns[k] - m->local_ns[k]) - yn);
  }
  double xm = sx / n, ym = sy / n;
  double sxx = 0, sxy = 0, syy = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t k = slot(m, i);
    double dx = (double)(m->local_ns[k] - xn) - xm;
    double dy = (double)((m->ref_ns[k] - m->local_ns[k]) - yn) - ym;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  double rate = sxy / sxx; /* sxx > 0: local times are strictly increasing */
  double ssr = syy - rate * sxy;
  if (ssr < 0) {
    ssr = 0;
  }
  f->rate = rate;
  f->y0_ns = m->ref_ns[last] + round_i64(ym - rate * xm); /* the line at x = 0 (the newest point) */
  f->resid_ns = sat_u32((float)sqrt(ssr / n));
  double s = n > 2 ? sqrt(ssr / (n - 2)) : TS_ASSUMED_JITTER_NS;
  f->rate_sigma_ppb = sat_u32((float)(s / sqrt(sxx) * 1e9));
}

static void reset_keep(struct ts_model *m, int64_t local_ns, int64_t ref_ns) {
  ts_model_reset(m);
  push(m, local_ns, ref_ns);
  recompute(m);
}

/*
 * Error estimate (ns, about one sigma) at local_ns. The rate error acts around the center of the window, not around
 * the newest point: the distance is taken from the window center (half the span before the newest point), so the
 * estimate at the newest point already includes the offset error of the fit there.
 */
static uint32_t uncertainty(const struct ts_fit *f, int64_t local_ns) {
  int64_t d = local_ns - f->x0_ns;
  float fd = (float)(d < 0 ? -d : d);
  float a = (float)f->resid_ns;
  float dc = (float)(local_ns - f->x0_ns + (f->last_local_ns - f->first_local_ns) / 2);
  float b = (float)f->rate_sigma_ppb * 1e-9f * (dc < 0 ? -dc : dc);
  float u = sqrtf(a * a / (float)f->points + b * b);
  if (d > 0) {
    u += (float)f->holdover_ppb * 1e-9f * fd;
  }
  return sat_u32(u);
}

int ts_fit_to_ref(const struct ts_fit *f, int64_t local_ns, int64_t *ref_ns, uint32_t *uncert_ns) {
  if (!f->valid) {
    return -EAGAIN;
  }
  int64_t d = local_ns - f->x0_ns;
  if (f->rate != 0) {
    *ref_ns = f->y0_ns + d + round_i64((double)d * f->rate);
  } else {
    *ref_ns = f->y0_ns + d;
  }
  if (uncert_ns != NULL) {
    *uncert_ns = uncertainty(f, local_ns);
  }
  return 0;
}

int ts_model_to_ref(const struct ts_model *m, int64_t local_ns, int64_t *ref_ns, uint32_t *uncert_ns) {
  return ts_fit_to_ref(&m->fit, local_ns, ref_ns, uncert_ns);
}

static int64_t fit_to_local(const struct ts_fit *f, int64_t ref_ns) {
  int64_t dr = ref_ns - f->y0_ns;
  if (f->rate == 0) {
    return f->x0_ns + dr;
  }
  return f->x0_ns + round_i64((double)dr / (1.0 + f->rate));
}

int ts_model_to_local(const struct ts_model *m, int64_t ref_ns, int64_t *local_ns) {
  if (!m->fit.valid) {
    return -EAGAIN;
  }
  *local_ns = fit_to_local(&m->fit, ref_ns);
  return 0;
}

void ts_model_fit_get(const struct ts_model *m, struct ts_fit *fit) { *fit = m->fit; }

enum ts_add_result ts_model_add(struct ts_model *m, int64_t local_ns, int64_t ref_ns) {
  if (m->count > 0 && local_ns <= m->local_ns[slot(m, (uint8_t)(m->count - 1))]) {
    return TS_ADD_INVALID;
  }

  /* The threshold is widened by the uncertainty of the extrapolation, so a long gap does not look like a step. */
  if (m->cfg.outlier_ns != 0 && m->fit.valid && m->count >= 2) {
    int64_t pred;
    uint32_t u;
    ts_fit_to_ref(&m->fit, local_ns, &pred, &u);
    int64_t err = ref_ns - pred;
    if (err < 0) {
      err = -err;
    }
    if (err > (int64_t)m->cfg.outlier_ns + (int64_t)u) {
      if (++m->outliers_in_row >= m->cfg.reset_after) {
        reset_keep(m, local_ns, ref_ns);
        return TS_ADD_RESET;
      }
      return TS_ADD_OUTLIER;
    }
  }
  m->outliers_in_row = 0;

  push(m, local_ns, ref_ns);
  recompute(m);
  if (m->count >= 2 && fabs(m->fit.rate) * 1e9 > (double)m->cfg.max_rate_ppb) {
    reset_keep(m, local_ns, ref_ns);
    return TS_ADD_RESET;
  }
  return TS_ADD_ACCEPTED;
}

int ts_fit_bridge(const struct ts_fit *before, const struct ts_fit *after, int64_t local_ns, int64_t *ref_ns) {
  if (!before->valid && !after->valid) {
    return -EAGAIN;
  }
  if (!before->valid) {
    return ts_fit_to_ref(after, local_ns, ref_ns, NULL);
  }
  if (!after->valid) {
    return ts_fit_to_ref(before, local_ns, ref_ns, NULL);
  }
  int64_t g0 = before->last_local_ns, g1 = after->first_local_ns;
  if (g1 <= g0) { /* no gap: the newer fit from its first point on */
    return ts_fit_to_ref(local_ns >= g1 ? after : before, local_ns, ref_ns, NULL);
  }
  if (local_ns <= g0) {
    return ts_fit_to_ref(before, local_ns, ref_ns, NULL);
  }
  if (local_ns >= g1) {
    return ts_fit_to_ref(after, local_ns, ref_ns, NULL);
  }
  int64_t p0, p1;
  ts_fit_to_ref(before, local_ns, &p0, NULL);
  ts_fit_to_ref(after, local_ns, &p1, NULL);
  double w = (double)(local_ns - g0) / (double)(g1 - g0);
  *ref_ns = p0 + round_i64((double)(p1 - p0) * w);
  return 0;
}
