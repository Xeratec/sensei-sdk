/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_model.h
 * @brief Linear clock model: maps a local clock to a reference clock.
 *
 * The model is fed with matched points (local_ns, ref_ns): the same physical instant read on the local clock
 * and on the reference clock (for BLE: one connection anchor point seen by the follower and by the master).
 * It fits ref = local + offset + rate * (local - x) by least squares over the last `window` points, rejects
 * outliers, detects steps of the reference (master reboot) and estimates its own uncertainty.
 *
 * The same model maps any monotonic counter to the reference time, for example the sample index of an audio
 * stream (scaled to nanoseconds at the nominal sample rate) to the global time.
 *
 * Pure logic: no OS, no locking, time is an argument. Tested on the host (tests/).
 */

#ifndef SENSEI_TS_MODEL_H_
#define SENSEI_TS_MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Capacity of the point buffer; cfg.window is at most this. */
#define TS_MODEL_MAX_POINTS 32

struct ts_model_cfg {
  /** Points used by the fit, 2 to TS_MODEL_MAX_POINTS. */
  uint8_t window;
  /** Points needed before the model is valid (1 or more). With one point the rate is 0. */
  uint8_t min_points;
  /**
   * A point farther than this from the prediction of a valid model is an outlier and is not used.
   * 0 disables the check.
   */
  uint32_t outlier_ns;
  /** This many outliers in a row reset the model to the newest point (a step of the reference). */
  uint8_t reset_after;
  /** A fit with |rate| above this (parts per billion) is not plausible: the model resets to the newest point. */
  uint32_t max_rate_ppb;
  /**
   * Assumed wander of the rate after the last point (parts per billion), for the uncertainty in holdover. The
   * 32.768 kHz crystals drift by about 0.035 ppm per degree squared away from 25 degrees C.
   */
  uint32_t holdover_ppb;
};

/** Result of ts_model_add(). */
enum ts_add_result {
  TS_ADD_ACCEPTED, /**< used by the fit */
  TS_ADD_OUTLIER,  /**< rejected, model unchanged */
  TS_ADD_RESET,    /**< the model was reset and now holds only this point */
  TS_ADD_INVALID,  /**< rejected: local time not after the previous point */
};

/** A fit, copied out of the model (for logging, or to bridge a gap with ts_fit_bridge()). */
struct ts_fit {
  bool valid;
  /** Number of points in the fit. */
  uint8_t points;
  /** Local time of the newest point, and the reference time the fit gives for it. */
  int64_t x0_ns;
  int64_t y0_ns;
  /** d(ref)/d(local) - 1, for example 12.5e-6 when the reference runs 12.5 ppm faster. */
  double rate;
  /** RMS residual of the points around the fit. */
  uint32_t resid_ns;
  /** Standard error of the rate (parts per billion). */
  uint32_t rate_sigma_ppb;
  /** Local time of the oldest and of the newest point of the fit. */
  int64_t first_local_ns;
  int64_t last_local_ns;
  /** Copy of cfg.holdover_ppb, for the uncertainty. */
  uint32_t holdover_ppb;
};

struct ts_model {
  struct ts_model_cfg cfg;
  int64_t local_ns[TS_MODEL_MAX_POINTS];
  int64_t ref_ns[TS_MODEL_MAX_POINTS];
  uint8_t head;  /* index of the next write */
  uint8_t count; /* points stored, at most cfg.window */
  uint8_t outliers_in_row;
  /** Incremented by every reset (also the internal ones). */
  uint32_t resets;
  struct ts_fit fit;
};

/** Defaults: window 16, min_points 2, outlier 50 us, reset after 3, max rate 500 ppm, holdover 1 ppm. */
void ts_model_cfg_default(struct ts_model_cfg *cfg);

/** cfg NULL takes the defaults. Out-of-range values are clamped. */
void ts_model_init(struct ts_model *m, const struct ts_model_cfg *cfg);

/** Forget all points (the configuration stays). */
void ts_model_reset(struct ts_model *m);

/**
 * Add a matched point. local_ns must be after the previous point (else TS_ADD_INVALID).
 * When the model is valid and has at least min_points, a point farther than cfg.outlier_ns from the
 * prediction is an outlier; cfg.reset_after outliers in a row reset the model to the newest point.
 */
enum ts_add_result ts_model_add(struct ts_model *m, int64_t local_ns, int64_t ref_ns);

/** True once the model holds at least cfg.min_points points. */
bool ts_model_valid(const struct ts_model *m);

/**
 * Reference time for a local time. uncert_ns (may be NULL) is an estimate of the error (about one sigma):
 * sqrt(resid^2 / n + (rate_sigma * dc)^2) + holdover * max(0, local - newest point), dc = distance to the
 * center of the window (the newest point minus half the span of the points). Works for times before the newest point
 * too (interpolation inside the window, extrapolation beyond).
 * @retval 0 on success, -EAGAIN if the model is not valid.
 */
int ts_model_to_ref(const struct ts_model *m, int64_t local_ns, int64_t *ref_ns, uint32_t *uncert_ns);

/** Local time for a reference time (inverse of ts_model_to_ref()). @retval 0, or -EAGAIN if not valid. */
int ts_model_to_local(const struct ts_model *m, int64_t ref_ns, int64_t *local_ns);

/** Copy of the current fit (fit->valid false if the model is not valid). */
void ts_model_fit_get(const struct ts_model *m, struct ts_fit *fit);

/** ts_model_to_ref() on a copied fit. */
int ts_fit_to_ref(const struct ts_fit *f, int64_t local_ns, int64_t *ref_ns, uint32_t *uncert_ns);

/**
 * Reference time for a local time inside a gap between two fits (holdover, for example while the link was down):
 * before is the fit at the start of the gap, after the fit once synchronization came back. Between
 * before->last_local_ns and after->first_local_ns the two predictions are blended linearly, so the error that
 * built up during the gap is spread over it instead of falling on the events at its end. Outside the gap the
 * nearer fit is used. Either fit may be invalid: then the other one is used alone.
 * @retval 0, or -EAGAIN if both are invalid.
 */
int ts_fit_bridge(const struct ts_fit *before, const struct ts_fit *after, int64_t local_ns, int64_t *ref_ns);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_TS_MODEL_H_ */
