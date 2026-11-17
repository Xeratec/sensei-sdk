/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_pwr_math.h"

#include <stddef.h>

struct battery_point {
  uint16_t mv;
  uint8_t perc;
};

/* Li-ion: the table of the first firmware. */
static const struct battery_point liion[] = {
    {4200, 100}, {4000, 85}, {3900, 60}, {3700, 40}, {3600, 20}, {3500, 10}, {3400, 5}, {3300, 0},
};

/* Li-SOCl2 (Saft LS33600, low current, room temperature): flat near 3.6 V, falls at the end. Rough. */
static const struct battery_point lisocl2[] = {
    {3650, 100}, {3600, 80}, {3500, 40}, {3300, 10}, {3000, 0},
};

uint8_t sensei_battery_perc(enum sensei_battery_chem chem, uint32_t mv) {
  const struct battery_point *t = chem == SENSEI_BATTERY_LIION ? liion : lisocl2;
  size_t n = chem == SENSEI_BATTERY_LIION ? sizeof(liion) / sizeof(liion[0]) : sizeof(lisocl2) / sizeof(lisocl2[0]);

  if (mv >= t[0].mv) {
    return 100;
  }
  if (mv <= t[n - 1].mv) {
    return 0;
  }
  for (size_t i = 1; i < n; i++) {
    if (mv > t[i].mv) {
      return (uint8_t)(t[i].perc + ((mv - t[i].mv) * (uint32_t)(t[i - 1].perc - t[i].perc)) / (t[i - 1].mv - t[i].mv));
    }
  }
  return 0;
}

void sensei_button_down(struct sensei_button *b, uint32_t now_ms) {
  /* A new key-down replaces an older one: its key-up was missed, and that press must not count. */
  b->pressed = true;
  b->down_ms = now_ms;
}

enum sensei_button_press sensei_button_up(struct sensei_button *b, uint32_t now_ms, uint32_t long_ms) {
  if (!b->pressed) {
    return SENSEI_BUTTON_NONE;
  }
  b->pressed = false;
  /* A key-up before the key-down time belongs to an older press (key-up read late): short. */
  int32_t held_ms = (int32_t)(now_ms - b->down_ms);

  return held_ms > 0 && (uint32_t)held_ms > long_ms ? SENSEI_BUTTON_LONG : SENSEI_BUTTON_SHORT;
}
