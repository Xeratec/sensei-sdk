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

static const struct g9_codec *codec_of(const struct g9 *g, const struct g9_entry *e) {
  return e->rec ? g->rec_codec : g->codec;
}

/* A recovery job never uses the camera bus. */
static bool uses_bus(const struct g9 *g, const struct g9_entry *e) { return !e->rec && g->codec->job_uses_bus(e->job); }

static uint32_t job_seq(const struct g9 *g, const struct g9_entry *e) { return codec_of(g, e)->job_seq(e->job); }

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

/* The end of g9_reboot(): once, through the platform. */
static void reboot_done(struct g9 *g, int err, const sensei_Version *hello) {
  g9_reboot_fn fn = g->reboot_fn;
  void *user = g->reboot_user;

  if (fn == NULL) {
    return;
  }
  g->reboot_fn = NULL;
  g->reboot_user = NULL;
  if (g->ops->reboot_done != NULL) {
    g->ops->reboot_done(fn, user, err, hello);
  }
}

/* GAP9 has the job: wait for its first answer. */
static void mark_sent(struct g9 *g, struct g9_entry *e) {
  g->grace = false;
  e->st = G9E_SENT;
  set_timer(g, uses_bus(g, e) ? G9T_BUSFREE : G9T_JOB, uses_bus(g, e) ? g->cfg.busfree_ms : g->cfg.job_ms);
}

/*
 * The job in flight. A BUSFREE or RESULT for the armed job that was never seen as taken (the POLL that took it was
 * garbled) shows that GAP9 has it: the job is recovered, not lost.
 */
static struct g9_entry *sent_or_recovered(struct g9 *g, uint32_t seq) {
  struct g9_entry *e = find(g, G9E_SENT);

  if (e == NULL && g->armed >= 0 && g->q[g->armed].st == G9E_PENDING && job_seq(g, &g->q[g->armed]) == seq) {
    e = &g->q[g->armed];
    mark_sent(g, e);
  }
  return e;
}

/* Put the frame for the next transaction into tx and arm the SPIS. Returns the frame kind. */
static void arm_frame(struct g9 *g, struct g9_entry *job) {
  uint8_t flags = g->err_flag ? G9L_FLAG_ERROR : 0;

  if (job != NULL) {
    int32_t len = codec_of(g, job)->encode_job(g->tx + G9L_HDR, G9L_TX_CAP - G9L_HDR, job->job);

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
}

/* Choose the frame: the job in flight (resend), else the oldest pending one, else NONE. */
static void arm_next(struct g9 *g, bool trigger) {
  struct g9_entry *e = NULL;

  if (g->state == G9_RUN) {
    e = find(g, G9E_SENT);
    if (e == NULL) {
      e = oldest_pending(g);
      if (e != NULL) {
        /* The bus lock is taken before GAP9 can see a detect job. */
        if (uses_bus(g, e) && !e->locked) {
          g->ops->bus_lock();
          e->locked = true;
        }
        /* GAP9 has to fetch the job. One that never does is power cycled (the timer is not
         * restarted by every poll or peek). */
        if (g->timer != G9T_FETCH) {
          set_timer(g, G9T_FETCH, g->cfg.job_ms);
        }
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

/* GAP9 is off first, then the camera bus goes back to the sensors. */
static void power_off(struct g9 *g) {
  if (g->recovery && g->rec_reason == G9L_RECOVERY_FORCED) {
    g->recovery = false; /* the strap goes with the power: the next boot is a normal one */
  }
  g->ops->trigger(false);
  g->ops->power(false);
  g->ops->marker(false);
  set_timer(g, G9T_NONE, 0);
  g->state = G9_OFF;
  g->armed = -1;
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    unlock_bus(g, &g->q[i]);
  }
}

static void fail_entry(struct g9 *g, struct g9_entry *e, int err) {
  g9_done_fn fn = e->done;
  void *user = e->user;

  unlock_bus(g, e);
  e->st = G9E_FREE;
  if (fn != NULL) {
    g->ops->deliver(fn, user, err, NULL, true);
  }
}

static void fail_all(struct g9 *g, int err) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st != G9E_FREE) {
      fail_entry(g, &g->q[i], err);
    }
  }
}

/* The jobs of one kind (app or recovery) that this GAP9 cannot run. */
static void fail_kind(struct g9 *g, bool rec, int err) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st != G9E_FREE && g->q[i].rec == rec) {
      fail_entry(g, &g->q[i], err);
    }
  }
}

/* The oldest GAP9 major that can run the job (0: any). */
static uint32_t job_min_major(const struct g9 *g, const struct g9_entry *e) {
  const struct g9_codec *c = codec_of(g, e);

  return c->job_min_major != NULL ? c->job_min_major(e->job) : 0;
}

/* Jobs of the app that the major of this GAP9 cannot run. */
static void fail_unsupported(struct g9 *g) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    struct g9_entry *e = &g->q[i];

    if (e->st != G9E_FREE && !e->rec && job_min_major(g, e) > g->version.major) {
      fail_entry(g, e, -EPROTONOSUPPORT);
    }
  }
}

/* After power_off() and the failed entry: wait, then boot again if jobs are left. */
static void restart(struct g9 *g) {
  if (oldest_pending(g) != NULL || g->hold || g->reboot_fn != NULL) {
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

bool g9_full(const struct g9 *g) {
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st == G9E_FREE) {
      return false;
    }
  }
  return true;
}

bool g9_busy(const struct g9 *g) {
  if (g->state != G9_OFF || g->hold || g->reboot_fn != NULL) {
    return true;
  }
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    if (g->q[i].st != G9E_FREE) {
      return true;
    }
  }
  return false;
}

static int submit(struct g9 *g, const void *job, g9_done_fn done, void *user, bool rec) {
  struct g9_entry *e = find(g, G9E_FREE);
  const struct g9_codec *c = rec ? g->rec_codec : g->codec;

  if (e == NULL) {
    return -ENOMEM;
  }
  memcpy(e->job, job, c->job_size);
  c->job_set_seq(e->job, ++g->seq);
  e->rec = rec;
  e->done = done;
  e->user = user;
  e->locked = false;
  e->busfree = false;
  e->partial = false;
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
      if (g->timer != G9T_FETCH) { /* a job that waits for its fetch keeps its timer */
        set_timer(g, G9T_NONE, 0);
      }
      arm_next(g, true);
    }
    break;
  default:
    break; /* booting or restarting: offered after HELLO */
  }
  return 0;
}

int g9_submit(struct g9 *g, const void *job, g9_done_fn done, void *user) {
  if (g->recovery) {
    return -ENOEXEC; /* the boot loader runs: no job of the app can, and GAP9 is not powered for it */
  }
  if (g->state == G9_RUN && g->codec->job_min_major != NULL && g->codec->job_min_major(job) > g->version.major) {
    return -EPROTONOSUPPORT; /* the app that runs is too old for this job (its HELLO is known) */
  }
  return submit(g, job, done, user, false);
}

int g9_set_recovery(struct g9 *g, const struct g9_codec *rec) {
  if (rec == NULL || rec->job_size > g->codec->job_size || rec->result_size > g->codec->result_size ||
      rec->encode_job == NULL || rec->decode_result == NULL || rec->job_seq == NULL || rec->job_set_seq == NULL ||
      rec->result_seq == NULL || rec->result_err == NULL) {
    return -EINVAL;
  }
  g->rec_codec = rec;
  return 0;
}

int g9_submit_recovery(struct g9 *g, const void *job, g9_done_fn done, void *user) {
  if (g->rec_codec == NULL) {
    return -ENOTSUP;
  }
  if (g->state == G9_RUN && !g->recovery) {
    return -ENOEXEC; /* GAP9 runs its app */
  }
  return submit(g, job, done, user, true);
}

bool g9_recovery(const struct g9 *g, uint32_t *reason) {
  if (reason != NULL) {
    *reason = g->recovery ? g->rec_reason : 0;
  }
  return g->recovery;
}

bool g9_app_major(const struct g9 *g, uint32_t *major) {
  if (g->recovery || g->version.major == 0) {
    return false;
  }
  if (major != NULL) {
    *major = g->version.major;
  }
  return true;
}

void g9_reprobe(struct g9 *g) {
  if (g->state != G9_OFF) {
    return; /* GAP9 is on (a session, a job): the latch is true for it, a power cycle would cut the transfer */
  }
  g->recovery = false;
  g->boot_retry = true;
  start_boot(g);
}

void g9_shutdown(struct g9 *g) {
  power_off(g);
  fail_all(g, -ECANCELED);
  g->hold = false;
  reboot_done(g, -ECANCELED, NULL);
}

int g9_reboot(struct g9 *g, g9_reboot_fn fn, void *user) {
  if (fn == NULL) {
    return -EINVAL;
  }
  if (g->reboot_fn != NULL) {
    return -EBUSY;
  }
  g->reboot_fn = fn;
  g->reboot_user = user;
  g->boot_retry = false; /* the same image would fail again */
  if (g->state != G9_OFF) {
    power_off(g);
  }
  fail_all(g, -ECANCELED);
  g->state = G9_CYCLE;
  set_timer(g, G9T_CYCLE, g->cfg.restart_off_ms);
  return 0;
}

void g9_hold(struct g9 *g, bool on) {
  g->hold = on;
  if (on) {
    if (g->state == G9_OFF) {
      g->boot_retry = true;
      start_boot(g);
    }
  } else if (g->state == G9_RUN && find(g, G9E_SENT) == NULL && oldest_pending(g) == NULL && g->timer != G9T_FETCH) {
    set_timer(g, G9T_IDLE, g->cfg.idle_off_ms);
  }
}

void g9_reset(struct g9 *g) {
  if (g->state == G9_OFF) {
    return;
  }
  struct g9_entry *e = find(g, G9E_SENT);

  power_off(g);
  if (e != NULL) {
    fail_entry(g, e, -ECANCELED);
  }
  restart(g);
}

/* Returns false if the frame ended the session (GAP9 is off now). */
static bool on_hello(struct g9 *g, const uint8_t *p, size_t len) {
  bool decoded = sensei_decode_Version(p, len, &g->version);
  bool rec = decoded && g->version.major == G9L_RECOVERY_MAJOR && g->rec_codec != NULL &&
             g->version.minor == g->rec_codec->gap9_major;
  uint32_t lo = g->codec->gap9_major_min != 0 ? g->codec->gap9_major_min : g->codec->gap9_major;

  if (!decoded || (!rec && (g->version.major < lo || g->version.major > g->codec->gap9_major))) {
    /* GAP9 runs a firmware with other job messages: no job can work. */
    count_error(g);
    power_off(g);
    fail_all(g, -EPROTO);
    g->hold = false;
    reboot_done(g, -EPROTO, decoded ? &g->version : NULL);
    return false;
  }
  /* Which program runs: the jobs of the other kind cannot. The app's jobs fail with -ENOEXEC in recovery (nothing is
   * powered for them again until the latch is cleared), the recovery jobs when the app came up. */
  g->recovery = rec;
  g->rec_reason = rec ? g->version.patch : 0;
  fail_kind(g, !rec, -ENOEXEC);
  if (!rec) {
    fail_unsupported(g); /* the jobs that only a newer app runs */
  }
  if (g->state == G9_RUN) {
    /* GAP9 restarted by itself (watchdog, fault): the job it had is lost. Run it again from the
     * start, unless some of its result frames were delivered already. */
    struct g9_entry *e = find(g, G9E_SENT);

    if (e != NULL) {
      count_error(g);
      if (e->partial) {
        fail_entry(g, e, -ECONNRESET);
      } else {
        e->st = G9E_PENDING;
        e->busfree = false;
      }
    }
  } else if (g->state == G9_BOOT) {
    g->boot_retry = true;
    g->state = G9_RUN;
    set_timer(g, G9T_NONE, 0);
    if (g->reboot_fn != NULL) {
      g->grace = true;
      reboot_done(g, rec ? -ENOEXEC : 0, &g->version); /* a trial that lands in recovery is a failed boot */
    }
  }
  return true;
}

static void on_busfree(struct g9 *g, const uint8_t *p, size_t len) {
  uint32_t seq = 0;

  if (len >= 4) {
    memcpy(&seq, p, 4); /* both sides are little endian */
  }
  struct g9_entry *e = sent_or_recovered(g, seq);

  if (e == NULL || !uses_bus(g, e) || job_seq(g, e) != seq || e->busfree) {
    count_error(g);
    return;
  }
  e->busfree = true;
  unlock_bus(g, e);
  set_timer(g, G9T_JOB, g->cfg.job_ms);
}

static void on_result(struct g9 *g, const uint8_t *p, size_t len, uint8_t flags) {
  struct g9_entry *e = NULL;
  bool more = (flags & G9L_FLAG_MORE) != 0;
  const struct g9_codec *c = g->recovery ? g->rec_codec : g->codec; /* GAP9 answers in the language it runs */

  memset(g->res, 0, c->result_size);
  if (c->decode_result(p, len, g->res)) {
    e = sent_or_recovered(g, c->result_seq(g->res));
  }
  if (e == NULL || e->rec != g->recovery || c->result_seq(g->res) != job_seq(g, e)) {
    count_error(g);
    return;
  }
  int err = c->result_err(g->res);

  if (uses_bus(g, e) && !e->busfree) {
    /* GAP9 is done with the camera. A successful job must say so first; a failed one (bad job,
     * detector not built) may answer at once. */
    if (err == 0) {
      count_error(g);
    }
    unlock_bus(g, e);
  }
  if (more) {
    /* first result of a JPEG job, or a chunk: the job goes on */
    e->partial = true;
    set_timer(g, G9T_JOB, g->cfg.job_ms);
    if (e->done != NULL) {
      g->ops->deliver(e->done, e->user, err, g->res, false);
    }
    return;
  }
  g9_done_fn fn = e->done;
  void *user = e->user;

  unlock_bus(g, e);
  e->st = G9E_FREE;
  if (err != 0) {
    count_error(g);
  }
  if (fn != NULL) {
    g->ops->deliver(fn, user, err, g->res, true);
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

  /* Any good frame from GAP9, a poll too, shows that the last header it got from us was fine. */
  g->err_flag = false;

  if (h.type == G9L_TYPE_POLL || h.type == G9L_TYPE_PEEK) {
    /* A peek reads the header only and never takes the frame, even a NONE of 8 bytes. */
    bool gap9_err = h.type == G9L_TYPE_POLL && (h.flags & G9L_FLAG_ERROR) != 0;
    bool consumed = h.type == G9L_TYPE_POLL && !gap9_err && rx_len >= g->tx_len;

    if (gap9_err) {
      count_error(g);
    }
    if (consumed && g->armed >= 0) {
      struct g9_entry *e = &g->q[g->armed];

      if (e->st == G9E_PENDING) {
        mark_sent(g, e);
      }
      arm_next(g, true); /* same job frame again, for a retry */
    } else if (consumed && g->armed < 0 && g->state == G9_RUN && find(g, G9E_SENT) == NULL) {
      /* "no more jobs" served: GAP9 sleeps, the idle timer decides the power-off */
      arm_next(g, false);
      set_timer(g, G9T_IDLE, g->grace && g->cfg.reboot_grace_ms != 0 ? g->cfg.reboot_grace_ms : g->cfg.idle_off_ms);
      g->grace = false;
    } else {
      arm_next(g, true);
    }
    return;
  }

  switch (h.type) {
  case G9L_TYPE_HELLO:
    if (!on_hello(g, payload, h.len)) {
      return;
    }
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
    if (g->reboot_fn != NULL) {
      power_off(g);
      fail_all(g, -ETIMEDOUT);
      g->hold = false;
      reboot_done(g, -ETIMEDOUT, NULL);
    } else if (g->boot_retry) {
      g->boot_retry = false;
      power_off(g);
      g->state = G9_CYCLE;
      set_timer(g, G9T_CYCLE, g->cfg.restart_off_ms);
    } else {
      power_off(g);
      fail_all(g, -ETIMEDOUT);
      g->hold = false;
    }
    break;
  case G9T_FETCH:
  case G9T_BUSFREE:
  case G9T_JOB: {
    /* FETCH: the armed job was never taken. Otherwise the job in flight. */
    struct g9_entry *e = kind == G9T_FETCH ? (g->armed >= 0 ? &g->q[g->armed] : NULL) : find(g, G9E_SENT);

    count_error(g);
    power_off(g);
    if (e != NULL) {
      fail_entry(g, e, -ETIMEDOUT);
    }
    restart(g);
    break;
  }
  case G9T_IDLE:
    if (!g->hold && find(g, G9E_SENT) == NULL && oldest_pending(g) == NULL) {
      power_off(g);
    }
    break;
  case G9T_CYCLE:
    if (oldest_pending(g) != NULL || g->hold || g->reboot_fn != NULL) {
      start_boot(g);
    } else {
      g->state = G9_OFF;
    }
    break;
  default:
    break;
  }
}
