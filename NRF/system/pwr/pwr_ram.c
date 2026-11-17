/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "pwr/pwr_ram.h"

#include <stdint.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <ram_pwrdn.h>

/* nRF5340 application core: 8 banks of 64 kB, power switch per 4 kB section (as the nRF Connect SDK library) */
#define RAM_START 0x20000000UL
#define RAM_END (RAM_START + 8UL * 0x10000UL)
#define SECTION 0x1000UL

#if DT_HAS_CHOSEN(zephyr_sram)
#define SRAM_END (DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) + DT_REG_SIZE(DT_CHOSEN(zephyr_sram)))
#else
#define SRAM_END RAM_END
#endif

extern char _image_ram_end[];

struct range {
  uintptr_t start, end;
};

#define KEEP_NODE(n) {DT_REG_ADDR(n), DT_REG_ADDR(n) + DT_REG_SIZE(n)},
#define KEEP_IF_REG(n) COND_CODE_1(DT_NODE_HAS_PROP(n, reg), (KEEP_NODE(n)), ())
#define KEEP_PARENT(n) KEEP_IF_REG(DT_PARENT(n))

/* RAM that is not part of the image but must keep its power */
static const struct range keep[] = {{0, 0}, /* keeps the array non-empty */
                                    DT_FOREACH_STATUS_OKAY(zephyr_memory_region, KEEP_IF_REG)
                                        DT_FOREACH_STATUS_OKAY(zephyr_retained_ram, KEEP_PARENT)
#if DT_HAS_CHOSEN(zephyr_ipc_shm)
                                            KEEP_NODE(DT_CHOSEN(zephyr_ipc_shm))
#endif
};

size_t sensei_pwr_ram_powerdown(void) {
  struct range gap[ARRAY_SIZE(keep) + 1];
  size_t n = 1;
  size_t bytes = 0;

  gap[0].start = (uintptr_t)_image_ram_end;
  gap[0].end = MIN((uintptr_t)SRAM_END, (uintptr_t)RAM_END);

  /* gap = [image end, RAM end) minus every keep range */
  for (size_t k = 0; k < ARRAY_SIZE(keep); k++) {
    size_t n0 = n;

    if (keep[k].start >= keep[k].end) {
      continue;
    }
    for (size_t i = 0; i < n0; i++) {
      struct range *g = &gap[i];

      if (keep[k].end <= g->start || keep[k].start >= g->end) {
        continue;
      }
      if (keep[k].start > g->start && keep[k].end < g->end) {
        gap[n].start = keep[k].end;
        gap[n].end = g->end;
        n++;
        g->end = keep[k].start;
      } else if (keep[k].start > g->start) {
        g->end = keep[k].start;
      } else if (keep[k].end < g->end) {
        g->start = keep[k].end;
      } else {
        g->end = g->start;
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    uintptr_t a = ROUND_UP(gap[i].start, SECTION);
    uintptr_t b = ROUND_DOWN(gap[i].end, SECTION);

    if (a < b) {
      power_down_ram(a, b);
      bytes += b - a;
    }
  }
  return bytes;
}
