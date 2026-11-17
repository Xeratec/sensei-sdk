/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_gap9/gap9_core.h"

#include <errno.h>
#include <string.h>

/* ---- helpers ---- */

static bool uses_bus(const struct g9 *g, const struct g9_entry *e) { return g->codec->job_uses_bus(e->job); }

static uint32_t job_seq(const struct g9 *g, const struct g9_entry *e) { return g->codec->job_seq(e->job); }

static struct g9_entry *find(struct g9 *g, int st) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if ((int)g->q[i].st == st) {
      return &g->q[i];
    }
  }
  return NULL;
}

/* FIFO: the oldest pending job has the lowest seq. */
static struct g9_entry *oldest_pending(struct g9 *g) {
  struct g9_entry *best = NULL;

  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    struct g9_entry *e = &g->q[i];

    if (e->st == G9E_PENDING && (best == NULL || (int32_t)(job_seq(g, e) - job_seq(g, best)) < 0)) {
      best = e;
    }
  }
  return best;
}

static void set_timer(struct g9 *g, int kind, uint32_t ms) {
  g->timer = kind;
  g->ops->timer(kind == G9T_NONE ? 0 : ms);
}

static void unlock_bus(struct g9 *g, struct g9_entry *e) {
  if (e->locked) {
    e->locked = false;
    g->ops->bus_unlock();
  }
}

static void count_error(struct g9 *g) {
  g->errors++;
  g->ops->error();
}

/* Put the frame for the next transaction into tx and arm the SPIS. Returns the frame kind. */
static void arm_frame(struct g9 *g, struct g9_entry *job) {
  uint8_t flags = g->err_flag ? G9L_FLAG_ERROR : 0;

  if (job != NULL) {
    int32_t len = g->codec->encode_job(g->tx + G9L_HDR, G9L_TX_CAP - G9L_HDR, job->job);

    if (len < 0) {
      /* cannot happen: the buffer fits the largest job */
      job = NULL;
    } else {
      size_t n = sensei_spi_frame_encode(G9L_TYPE_JOB, flags, g->tx + G9L_HDR, (size_t)len, g->tx, G9L_TX_CAP);
      g->armed = (int)(job - g->q);
      g->tx_len = n;
    }
  }
  if (job == NULL) {
    sensei_spi_header_pack(g->tx, G9L_TYPE_NONE, flags, NULL, 0);
    g->armed = -1;
    g->tx_len = G9L_HDR;
  }
  g->ops->arm(g->tx, g->tx_len);
}

/* Choose the frame: the job in flight (resend), else the oldest pending one, else NONE. */
static void arm_next(struct g9 *g, bool trigger) {
  struct g9_entry *e = NULL;

  if (g->state == G9_RUN) {
    e = find(g, G9E_SENT);
    if (e == NULL) {
      e = oldest_pending(g);
      /* The bus lock is taken before GAP9 can see a detect job. */
      if (e != NULL && uses_bus(g, e) && !e->locked) {
        g->ops->bus_lock();
        e->locked = true;
      }
    }
  }
  arm_frame(g, e);
  g->ops->trigger(trigger);
}

static void start_boot(struct g9 *g) {
  g->state = G9_BOOT;
  g->err_flag = false;
  g->ops->power(true);
  g->ops->marker(true);
  arm_next(g, true);
  set_timer(g, G9T_BOOT, g->cfg.boot_ms);
}

static void power_off(struct g9 *g) {
  g->ops->trigger(false);
  g->ops->power(false);
  g->ops->marker(false);
  set_timer(g, G9T_NONE, 0);
  g->state = G9_OFF;
  g->armed = -1;
}

static void fail_entry(struct g9 *g, struct g9_entry *e, int err) {
  g9_done_fn fn = e->done;
  void *user = e->user;

  unlock_bus(g, e);
  e->st = G9E_FREE;
  if (fn != NULL) {
    g->ops->deliver(fn, user, err, NULL);
  }
}

static void fail_all(struct g9 *g, int err) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st != G9E_FREE) {
      fail_entry(g, &g->q[i], err);
    }
  }
}

/* Power cycle: off, wait, boot again if jobs are left. */
static void cycle(struct g9 *g) {
  power_off(g);
  if (oldest_pending(g) != NULL) {
    g->state = G9_CYCLE;
    set_timer(g, G9T_CYCLE, g->cfg.restart_off_ms);
  }
}

/* ---- public ---- */

void g9_init(struct g9 *g, const struct g9_ops *ops, const struct g9_codec *codec, const struct g9_cfg *cfg, void *jobs,
             void *res) {
  memset(g, 0, sizeof(*g));
  g->ops = ops;
  g->codec = codec;
  g->cfg = *cfg;
  g->armed = -1;
  g->state = G9_OFF;
  g->res = res;
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    g->q[i].job = (uint8_t *)jobs + (size_t)i * codec->job_size;
  }
}

bool g9_busy(const struct g9 *g) {
  if (g->state != G9_OFF) {
    return true;
  }
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st != G9E_FREE) {
      return true;
    }
  }
  return false;
}

int g9_submit(struct g9 *g, const void *job, g9_done_fn done, void *user) {
  struct g9_entry *e = find(g, G9E_FREE);

  if (e == NULL) {
    return -ENOMEM;
  }
  memcpy(e->job, job, g->codec->job_size);
  g->codec->job_set_seq(e->job, ++g->seq);
  e->done = done;
  e->user = user;
  e->locked = false;
  e->busfree = false;
  e->st = G9E_PENDING;

  switch (g->state) {
  case G9_OFF:
    g->boot_retry = true;
    start_boot(g);
    break;
  case G9_RUN:
    /* GAP9 is idle (trigger low, no job in flight): wake it up. A job in flight keeps the
     * armed frame; the new job is offered after its result. */
    if (find(g, G9E_SENT) == NULL) {
      set_timer(g, G9T_NONE, 0);
      arm_next(g, true);
    }
    break;
  default:
    break; /* booting or restarting: offered after HELLO */
  }
  return 0;
}

void g9_shutdown(struct g9 *g) {
  power_off(g);
  fail_all(g, -ECANCELED);
}

static void on_hello(struct g9 *g, const uint8_t *p, size_t len) {
  if (!sensei_decode_Version(p, len, &g->version)) {
    memset(&g->version, 0, sizeof(g->version));
  }
  if (g->state == G9_BOOT) {
    g->boot_retry = true;
    g->state = G9_RUN;
    set_timer(g, G9T_NONE, 0);
  }
}

static void on_busfree(struct g9 *g, const uint8_t *p, size_t len) {
  struct g9_entry *e = find(g, G9E_SENT);
  uint32_t seq = 0;

  if (len >= 4) {
    memcpy(&seq, p, 4); /* both sides are little endian */
  }
  if (e == NULL || !uses_bus(g, e) || job_seq(g, e) != seq || e->busfree) {
    count_error(g);
    return;
  }
  e->busfree = true;
  unlock_bus(g, e);
  set_timer(g, G9T_JOB, g->cfg.job_ms);
}

static void on_result(struct g9 *g, const uint8_t *p, size_t len, uint8_t flags) {
  struct g9_entry *e = find(g, G9E_SENT);
  bool more = (flags & G9L_FLAG_MORE) != 0;

  memset(g->res, 0, g->codec->result_size);
  if (!g->codec->decode_result(p, len, g->res) || e == NULL || g->codec->result_seq(g->res) != job_seq(g, e)) {
    count_error(g);
    return;
  }
  if (uses_bus(g, e) && !e->busfree) {
    count_error(g); /* result before bus free: GAP9 broke the order */
    unlock_bus(g, e);
  }
  if (more) {
    /* first result of a JPEG job, or a chunk: the job goes on */
    set_timer(g, G9T_JOB, g->cfg.job_ms);
    if (e->done != NULL) {
      g->ops->deliver(e->done, e->user, g->codec->result_err(g->res), g->res);
    }
    return;
  }
  g9_done_fn fn = e->done;
  void *user = e->user;

  unlock_bus(g, e);
  e->st = G9E_FREE;
  int err = g->codec->result_err(g->res);

  if (err != 0) {
    count_error(g);
  }
  if (fn != NULL) {
    g->ops->deliver(fn, user, err, g->res);
  }
}

void g9_spi_done(struct g9 *g, const uint8_t *rx, size_t rx_len) {
  sensei_spi_header_t h;
  const uint8_t *payload = NULL;

  if (g->state != G9_BOOT && g->state != G9_RUN) {
    return; /* stray transaction while off */
  }
  if (rx_len == 0) {
    return; /* CS glitch */
  }
  g->ops->trigger(false); /* busy until re-armed */

  if (sensei_spi_frame_decode(rx, rx_len, &h, &payload) != SENSEI_SPI_OK) {
    g->err_flag = true;
    count_error(g);
    arm_next(g, true);
    return;
  }

  if (h.type == G9L_TYPE_POLL) {
    bool gap9_err = (h.flags & G9L_FLAG_ERROR) != 0;
    bool consumed = !gap9_err && rx_len >= g->tx_len;

    if (gap9_err) {
      count_error(g);
    }
    if (consumed && g->armed >= 0) {
      struct g9_entry *e = &g->q[g->armed];

      if (e->st == G9E_PENDING) {
        e->st = G9E_SENT;
        set_timer(g, uses_bus(g, e) ? G9T_BUSFREE : G9T_JOB, uses_bus(g, e) ? g->cfg.busfree_ms : g->cfg.job_ms);
      }
      arm_next(g, true); /* same job frame again, for a retry */
    } else if (consumed && g->armed < 0 && g->state == G9_RUN && find(g, G9E_SENT) == NULL) {
      /* "no more jobs" served: GAP9 sleeps, the idle timer decides the power-off */
      arm_next(g, false);
      set_timer(g, G9T_IDLE, g->cfg.idle_off_ms);
    } else {
      arm_next(g, true);
    }
    return;
  }

  /* A frame from GAP9: it is good. */
  g->err_flag = false;
  switch (h.type) {
  case G9L_TYPE_HELLO:
    on_hello(g, payload, h.len);
    break;
  case G9L_TYPE_BUSFREE:
    on_busfree(g, payload, h.len);
    break;
  case G9L_TYPE_RESULT:
    on_result(g, payload, h.len, h.flags);
    break;
  default:
    g->err_flag = true;
    count_error(g);
    break;
  }
  arm_next(g, true);
}

void g9_timer_expired(struct g9 *g) {
  int kind = g->timer;

  g->timer = G9T_NONE;
  switch (kind) {
  case G9T_BOOT:
    count_error(g);
    if (g->boot_retry) {
      g->boot_retry = false;
      power_off(g);
      g->state = G9_CYCLE;
      set_timer(g, G9T_CYCLE, g->cfg.restart_off_ms);
    } else {
      power_off(g);
      fail_all(g, -ETIMEDOUT);
    }
    break;
  case G9T_BUSFREE:
  case G9T_JOB: {
    struct g9_entry *e = find(g, G9E_SENT);

    count_error(g);
    if (e != NULL) {
      fail_entry(g, e, -ETIMEDOUT);
    }
    cycle(g);
    break;
  }
  case G9T_IDLE:
    if (find(g, G9E_SENT) == NULL && oldest_pending(g) == NULL) {
      power_off(g);
    }
    break;
  case G9T_CYCLE:
    if (oldest_pending(g) != NULL) {
      start_boot(g);
    } else {
      g->state = G9_OFF;
    }
    break;
  default:
    break;
  }
}
