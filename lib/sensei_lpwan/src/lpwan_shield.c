/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <stddef.h>

#include "sensei_lpwan/lpwan_shield.h"

static enum lpwan_shield_state fault(const struct lpwan_shield_ops *ops, void *ctx) {
  ops->power_cut(ctx);
  return LPWAN_SHIELD_FAULT;
}

enum lpwan_shield_state lpwan_shield_start(const struct lpwan_shield_ops *ops, void *ctx) {
  bool pins_ok = false;

  if (!ops->expander_answers(ctx)) {
    ops->release(ctx);
    return LPWAN_SHIELD_ABSENT;
  }
  if (!ops->devices_ready(ctx)) {
    return fault(ops, ctx);
  }
  for (int i = 0; i < LPWAN_SHIELD_PIN_TRIES && !pins_ok; i++) {
    if (i > 0 && ops->pause != NULL) {
      ops->pause(ctx);
    }
    pins_ok = ops->pins_init(ctx) == 0;
  }
  if (!pins_ok || ops->irq_setup(ctx) != 0) {
    return fault(ops, ctx);
  }
  return LPWAN_SHIELD_PRESENT;
}

const char *lpwan_shield_name(enum lpwan_shield_state s) {
  switch (s) {
  case LPWAN_SHIELD_PRESENT:
    return "present";
  case LPWAN_SHIELD_ABSENT:
    return "absent";
  case LPWAN_SHIELD_FAULT:
    return "fault";
  default:
    return "unknown";
  }
}

struct lpwan_fem_levels lpwan_fem_levels(bool fem_in_use, bool shield_on, bool mode_high) {
  struct lpwan_fem_levels l = {.npd_high = false, .mode_high = false};

  if (fem_in_use) {
    l.npd_high = shield_on;
    l.mode_high = mode_high;
  }
  return l;
}
