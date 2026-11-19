/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file measure.c
 * @brief Master: match follower reports to the own capture of the same edge and keep statistics of the error.
 *
 * The edge carries no data. A report belongs to the master capture closest in global time, if it is within
 * CONFIG_APP_MATCH_WINDOW_MS. The error is follower global time minus master global time: positive means the
 * follower clock is ahead.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_DECLARE(app, CONFIG_APP_LOG_LEVEL);

#define CAPTURE_RING 16
#define WINDOW_NS ((int64_t)CONFIG_APP_MATCH_WINDOW_MS * 1000000)

struct stat_entry {
  bool used;
  char addr[BT_ADDR_LE_STR_LEN];
  uint32_t n;          /* matched, synced reports in the statistics */
  uint32_t missed;     /* reports missing in the sequence of the follower */
  uint32_t not_synced; /* reports of a follower that was not SYNCED */
  uint32_t unmatched;  /* synced reports without a master capture in the window */
  uint32_t last_seq;
  bool have_seq;
  uint32_t last_uncert_ns;
  double mean;
  double m2; /* sum of squared differences from the mean (Welford) */
  int64_t min;
  int64_t max;
};

static struct k_spinlock lock;
static uint64_t ring[CAPTURE_RING];
static uint8_t ring_fill;
static uint8_t ring_head;
static struct stat_entry stats[CONFIG_BT_MAX_CONN];

void measure_capture_add(uint64_t global_ns) {
  K_SPINLOCK(&lock) {
    ring[ring_head] = global_ns;
    ring_head = (ring_head + 1) % CAPTURE_RING;
    if (ring_fill < CAPTURE_RING) {
      ring_fill++;
    }
  }
}

/* Master capture closest to ns within the window (lock held). */
static bool find_capture(uint64_t ns, uint64_t *master_ns) {
  bool found = false;
  int64_t best = WINDOW_NS;

  for (uint8_t i = 0; i < ring_fill; i++) {
    int64_t d = (int64_t)(ns - ring[i]);

    if (d < 0) {
      d = -d;
    }
    if (d < best) {
      best = d;
      *master_ns = ring[i];
      found = true;
    }
  }
  return found;
}

static void print_entry(uint8_t idx, const struct stat_entry *s) {
  if (s->n == 0) {
    LOG_INF("F%u %s: no matches yet (not synced %u, unmatched %u, missed %u)", idx, s->addr, s->not_synced,
            s->unmatched, s->missed);
    return;
  }
  long long std = s->n > 1 ? llround(sqrt(s->m2 / (s->n - 1))) : 0;

  LOG_INF("F%u %s: n %u, mean %lld ns, std %lld ns, min %lld ns, max %lld ns, uncert %u ns, missed %u, "
          "not synced %u, unmatched %u",
          idx, s->addr, s->n, llround(s->mean), std, (long long)s->min, (long long)s->max, s->last_uncert_ns, s->missed,
          s->not_synced, s->unmatched);
}

void measure_follower_add(uint8_t idx, const bt_addr_le_t *addr) {
  if (idx >= ARRAY_SIZE(stats)) {
    return;
  }
  K_SPINLOCK(&lock) {
    memset(&stats[idx], 0, sizeof(stats[idx]));
    stats[idx].used = true;
    bt_addr_le_to_str(addr, stats[idx].addr, sizeof(stats[idx].addr));
  }
}

void measure_follower_remove(uint8_t idx) {
  struct stat_entry copy;

  if (idx >= ARRAY_SIZE(stats)) {
    return;
  }
  K_SPINLOCK(&lock) {
    copy = stats[idx];
    stats[idx].used = false;
  }
  if (copy.used) {
    print_entry(idx, &copy);
  }
}

void measure_report(uint8_t idx, const struct app_report *r) {
  struct stat_entry copy = {0};
  bool print = false;
  bool matched = false;
  int64_t err = 0;

  if (idx >= ARRAY_SIZE(stats)) {
    return;
  }

  K_SPINLOCK(&lock) {
    struct stat_entry *s = &stats[idx];
    uint64_t master_ns;

    if (!s->used) {
      K_SPINLOCK_BREAK;
    }

    if (s->have_seq) {
      uint32_t gap = r->seq - s->last_seq - 1;

      /* A large gap is a restart of the follower, not lost reports. */
      if (gap > 0 && gap < 1000) {
        s->missed += gap;
      }
    }
    s->have_seq = true;
    s->last_seq = r->seq;
    s->last_uncert_ns = r->uncert_ns;

    if (r->state != SENSEI_TS_SYNCED) {
      s->not_synced++;
    } else if (!find_capture(r->global_ns, &master_ns)) {
      s->unmatched++;
    } else {
      double d;

      err = (int64_t)(r->global_ns - master_ns);
      matched = true;
      if (s->n == 0 || err < s->min) {
        s->min = err;
      }
      if (s->n == 0 || err > s->max) {
        s->max = err;
      }
      s->n++;
      d = (double)err - s->mean;
      s->mean += d / s->n;
      s->m2 += d * ((double)err - s->mean);
      print = (s->n % CONFIG_APP_STATS_EVERY) == 0;
    }
    copy = *s;
  }

  if (matched) {
    LOG_DBG("F%u seq %u: error %lld ns (follower uncert %u ns)", idx, r->seq, (long long)err, r->uncert_ns);
  } else {
    LOG_DBG("F%u seq %u: not used (state %s)", idx, r->seq, app_state_str(r->state));
  }
  if (print) {
    print_entry(idx, &copy);
  }
}

void measure_print_all(void) {
  struct stat_entry copy[CONFIG_BT_MAX_CONN];

  K_SPINLOCK(&lock) { memcpy(copy, stats, sizeof(copy)); }
  bool any = false;

  for (uint8_t i = 0; i < ARRAY_SIZE(copy); i++) {
    if (copy[i].used) {
      print_entry(i, &copy[i]);
      any = true;
    }
  }
  if (!any) {
    LOG_INF("No follower connected");
  }
}

void measure_reset_all(void) {
  K_SPINLOCK(&lock) {
    for (uint8_t i = 0; i < ARRAY_SIZE(stats); i++) {
      if (stats[i].used) {
        char addr[BT_ADDR_LE_STR_LEN];

        memcpy(addr, stats[i].addr, sizeof(addr));
        memset(&stats[i], 0, sizeof(stats[i]));
        stats[i].used = true;
        memcpy(stats[i].addr, addr, sizeof(addr));
      }
    }
  }
}
