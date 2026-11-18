/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_shield.h"

enum lpwan_shield_state lpwan_shield_probe(bool devices_ready, bool pins_ok) {
  return devices_ready && pins_ok ? LPWAN_SHIELD_PRESENT : LPWAN_SHIELD_ABSENT;
}

const char *lpwan_shield_name(enum lpwan_shield_state s) {
  switch (s) {
  case LPWAN_SHIELD_PRESENT:
    return "present";
  case LPWAN_SHIELD_ABSENT:
    return "absent";
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
