/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_uplink/uplink_sched.h"

#include <errno.h>
#include <string.h>

#include "sensei_link/frag.h"

static struct uplink_sched *the_sched; /* the radio's done() only carries the send id */

void us_init(struct uplink_sched *s, uint8_t *qbuf, size_t qcap, uint8_t *abuf, size_t acap, const struct us_ops *ops,
             void *ctx) {
  memset(s, 0, sizeof(*s));
  mq_init(&s->q, qbuf, qcap);
  mq_init(&s->qa, abuf, acap);
  s->cur_q = &s->q;
  s->fail_q = &s->q;
  s->ops = ops;
  s->ctx = ctx;
  s->radio = sensei_Radio_RADIO_BLE;
  the_sched = s;
}

void us_set_config(struct uplink_sched *s, sensei_Radio radio, uint8_t retries) {
  s->radio = radio;
  s->retries = retries;
}

void us_set_defer(struct uplink_sched *s, uint32_t max_ms) {
  s->defer_max_ms = max_ms;
  if (max_ms == 0) {
    s->deferring = false;
  }
}

bool us_defer_ok(bool urgent, uint32_t max_ms, bool deferring, uint32_t since_ms, uint32_t now_ms, size_t q_used,
                 size_t q_cap) {
  if (urgent || max_ms == 0 || q_used * 2 >= q_cap) {
    return false;
  }
  return !deferring || (uint32_t)(now_ms - since_ms) < max_ms;
}

int us_register(struct uplink_sched *s, const struct uplink_radio *r) {
  if (s->n_radios >= US_MAX_RADIOS) {
    return -ENOSPC;
  }
  s->radios[s->n_radios++] = r;
  return 0;
}

static const struct uplink_radio *find(const struct uplink_sched *s, sensei_Radio id) {
  for (size_t i = 0; i < s->n_radios; i++) {
    if (s->radios[i]->id == id) {
      return s->radios[i];
    }
  }
  return NULL;
}

const struct uplink_radio *us_select(const struct uplink_sched *s) {
  const struct uplink_radio *r = find(s, s->radio);

  return r ? r : find(s, sensei_Radio_RADIO_BLE);
}

static void end_session(struct uplink_sched *s) {
  s->state = US_IDLE;
  s->cur_radio = NULL;
  s->no_check = false;
  s->send_id++; /* a completion of the send in flight is stale from now on */
}

/* Remove the oldest message of q and tell the app if it is a report or holds a key mix request. */
static void pop_head(struct uplink_sched *s, struct msg_queue *q, bool delivered) {
  uint8_t flags = mq_peek_flags(q);

  mq_pop(q);
  s->conn_defers = 0; /* the next head starts clean */
  if ((flags & (MQ_FLAG_REPORT | MQ_FLAG_REKEY)) && s->ops->msg_end != NULL) {
    s->ops->msg_end(s->ctx, flags, delivered);
  }
}

/* The queue is full: drop its oldest message. If that one is being sent, the session ends. */
static void evict_oldest(struct uplink_sched *s, struct msg_queue *q) {
  if (s->state != US_IDLE && s->cur_q == q) {
    end_session(s);
  }
  pop_head(s, q, false);
  if (s->fail_q == q) {
    s->head_fails = 0;
  }
  s->stats.msgs_evicted++;
  s->ops->error(s->ctx, US_ERR_QUEUE_FULL);
}

static int submit(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert, uint8_t flags) {
  struct msg_queue *q = alert ? &s->qa : &s->q;

  if (len == 0 || len > US_MAX_MSG) {
    return -EINVAL;
  }
  if (len + MQ_HDR > q->cap) {
    q->dropped++;
    s->ops->error(s->ctx, US_ERR_QUEUE_FULL);
    return -ENOMEM;
  }
  while (mq_free(q) < len + MQ_HDR) {
    evict_oldest(s, q);
  }
  return mq_push_flags(q, enc, len, flags);
}

int us_submit(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert) {
  return submit(s, enc, len, alert, 0);
}

int us_submit_plain(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert) {
  return submit(s, enc, len, alert, MQ_FLAG_PLAIN);
}

int us_submit_flags(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert, uint8_t flags) {
  return submit(s, enc, len, alert, flags);
}

bool us_radio_ready(const struct uplink_sched *s) {
  const struct uplink_radio *r = us_select(s);

  return r != NULL && r->ready();
}

void us_queued(const struct uplink_sched *s, size_t *msgs, size_t *bytes) {
  *msgs = s->q.count + s->qa.count;
  *bytes = s->q.used + s->qa.used;
}

static uint32_t now_ms(const struct uplink_sched *s) { return s->ops->now_ms(s->ctx); }

static void begin_next(struct uplink_sched *s);
static void send_chunk(struct uplink_sched *s);

/* The chunk failed for good: keep the message for the next time, drop it after repeated fails. */
static void session_failed(struct uplink_sched *s) {
  s->ops->error(s->ctx, US_ERR_RADIO);
  if (s->fail_q != s->cur_q) {
    s->fail_q = s->cur_q;
    s->head_fails = 0;
  }
  if (++s->head_fails >= US_MAX_SESSION_FAILS) {
    pop_head(s, s->cur_q, false);
    s->head_fails = 0;
    s->stats.msgs_dropped++;
  }
  s->attempts = 0; /* the next session starts with all its attempts */
  end_session(s);
}

uint32_t us_retry_delay_ms(uint8_t attempt, uint32_t rnd) {
  uint32_t delay = US_RETRY_BASE_MS << (attempt < 5 ? attempt : 5);

  if (delay > US_RETRY_MAX_MS) {
    delay = US_RETRY_MAX_MS;
  }
  return delay + rnd % (delay / 2 + 1);
}

static uint32_t msg_hash(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261u; /* FNV-1a */

  while (n--) {
    h = (h ^ *p++) * 16777619u;
  }
  return h;
}

/* The radio cannot send now (-EAGAIN: LoRa duty cycle, a shield update; -ENOTCONN: the shield
 * restarted and connects again). It calls on_up when it can, which starts the queue again. The
 * message stays. A deferral for the duty cycle counts nothing against it. -ENOTCONN is counted per
 * head message without progress: after US_MAX_CONN_DEFERS the session counts as failed, so a shield
 * that restarts on every send ends in a drop, as before.
 *
 * The next session goes on with the chunk that was refused only if the radio keeps the collector's
 * partial message (keeps_reasm) and the last chunk is recent (see begin_next()): the chunks before
 * it are out and each costs duty cycle again (at DR0 with confirmed uplinks every chunk after the
 * first is refused once for a few seconds, so a message that starts over each time never gets past
 * chunk 0). */
static void defer_message(struct uplink_sched *s, bool not_connected) {
  s->stats.deferred++;
  if (s->chunk_idx > 0) {
    s->resume = true;
    s->resume_radio = s->cur_radio;
    s->resume_msg_id = s->msg_id;
    s->resume_idx = s->chunk_idx;
    s->resume_cnt = s->chunk_cnt;
    s->resume_len = s->cur_len;
    s->resume_mtu = s->cur_mtu;
    s->resume_hash = msg_hash(s->cur, s->cur_len);
    s->resume_ms = s->last_ok_ms;
  }
  if (not_connected) {
    s->stats.conn_deferred++;
    if (s->fail_q != s->cur_q) {
      s->fail_q = s->cur_q;
      s->head_fails = 0;
      s->conn_defers = 0;
    }
    if (s->conn_defers < UINT8_MAX) {
      s->conn_defers++;
    }
    if (s->conn_defers >= US_MAX_CONN_DEFERS) {
      session_failed(s);
      return;
    }
  }
  end_session(s);
}

static void chunk_failed(struct uplink_sched *s) {
  uint32_t delay;

  if (s->attempts >= s->retries) {
    session_failed(s);
    return;
  }
  delay = us_retry_delay_ms(s->attempts, s->ops->rand ? s->ops->rand(s->ctx) : 0);
  s->progress_ms = now_ms(s);
  s->send_id++;
  s->attempts++;
  s->stats.retries++;
  s->state = US_BACKOFF;
  s->ops->schedule(s->ctx, delay);
}

static size_t radio_mtu(const struct uplink_radio *r) {
  size_t mtu = r->mtu_now ? r->mtu_now() : r->mtu;

  return mtu < r->mtu ? mtu : r->mtu; /* .mtu is the maximum */
}

/* The radio said a chunk does not fit (-EMSGSIZE). If its chunk size changed, chunk the whole
 * message again with the size now. If not, the radio's answer is no use for chunking: a failed
 * chunk, with the backoff (else the same chunk goes out again at once). */
static void restart_message(struct uplink_sched *s) {
  size_t mtu = radio_mtu(s->cur_radio);

  if (mtu == s->cur_mtu) {
    chunk_failed(s);
    return;
  }
  if (s->attempts >= s->retries || mtu > US_MAX_MTU) {
    session_failed(s);
    return;
  }
  s->attempts++;
  s->stats.retries++;
  s->progress_ms = now_ms(s);
  s->chunk_cnt = sensei_frag_count(s->cur_len, mtu);
  if (s->chunk_cnt == 0) { /* does not fit even now: drop it, as begin_next() does */
    pop_head(s, s->cur_q, false);
    if (s->fail_q == s->cur_q) {
      s->head_fails = 0;
    }
    s->stats.msgs_dropped++;
    s->ops->error(s->ctx, US_ERR_RADIO);
    begin_next(s);
    return;
  }
  s->cur_mtu = mtu;
  s->msg_id++;
  s->chunk_idx = 0;
  s->unconf_retried = false;
  send_chunk(s);
}

/* The chunk is out (and answered, if the radio waited for it). */
static void chunk_ok(struct uplink_sched *s) {
  s->progress_ms = now_ms(s);
  s->last_ok_ms = s->progress_ms;
  s->stats.chunks_sent++;
  s->attempts = 0;
  s->conn_defers = 0; /* progress */
  if (++s->chunk_idx < s->chunk_cnt) {
    send_chunk(s);
    return;
  }
  pop_head(s, s->cur_q, true);
  if (s->fail_q == s->cur_q) {
    s->head_fails = 0;
  }
  s->stats.msgs_sent++;
  s->msg_id++;
  s->unconf_retried = false;
  begin_next(s);
}

/* The last chunk went out and the answer did not come: send it once more in this session (the
 * collector answers a repeated last chunk again), then count the message as sent. */
static void chunk_unconfirmed(struct uplink_sched *s) {
  if (s->chunk_idx + 1 == s->chunk_cnt && !s->unconf_retried) {
    s->unconf_retried = true;
    s->progress_ms = now_ms(s);
    s->send_id++;
    s->stats.unconf_retries++;
    s->state = US_BACKOFF;
    s->ops->schedule(s->ctx, us_retry_delay_ms(0, s->ops->rand ? s->ops->rand(s->ctx) : 0));
    return;
  }
  s->stats.unconfirmed++;
  chunk_ok(s);
}

static void chunk_done(int err, void *user) {
  struct uplink_sched *s = the_sched;

  if (s->state != US_SENDING || (uint32_t)(uintptr_t)user != s->send_id) {
    return; /* aborted, timed out or retried meanwhile */
  }
  if (err == -EAGAIN || err == -ENOTCONN) {
    defer_message(s, err == -ENOTCONN);
    return;
  }
  if (err == -EMSGSIZE) {
    restart_message(s);
    return;
  }
  if (err == US_SENT_UNCONFIRMED) {
    chunk_unconfirmed(s);
    return;
  }
  if (err) {
    chunk_failed(s);
    return;
  }
  chunk_ok(s);
}

static void send_chunk(struct uplink_sched *s) {
  const struct uplink_radio *r = s->cur_radio;
  size_t n = sensei_frag_chunk(s->msg_id, s->cur, s->cur_len, s->cur_mtu, s->chunk_idx, s->chunk, sizeof(s->chunk));
  uint32_t now = now_ms(s);
  uint32_t id;
  int err;

  s->state = US_SENDING;
  if (n == 0) {
    session_failed(s);
    return;
  }
  id = ++s->send_id;
  s->progress_ms = now;
  s->cur_timeout_ms = r->chunk_timeout_ms ? r->chunk_timeout_ms : US_CHUNK_TIMEOUT_MS;
  s->deadline_ms = now + s->cur_timeout_ms;
  /* Before send(): a radio may call done() from inside send(). */
  s->ops->schedule(s->ctx, s->cur_timeout_ms);
  if (r->send_last != NULL) {
    /* The last of the session: the last chunk of the last queued message (this one is still
     * in its queue). */
    bool last = s->chunk_idx + 1 == s->chunk_cnt && s->q.count + s->qa.count == 1;

    err = r->send_last(s->chunk, n, last, chunk_done, (void *)(uintptr_t)id);
  } else {
    err = r->send(s->chunk, n, chunk_done, (void *)(uintptr_t)id);
  }
  if (err < 0) {
    /* Refused now; no done() will follow. */
    if (s->state == US_SENDING && id == s->send_id) {
      if (err == -EAGAIN || err == -ENOTCONN) {
        defer_message(s, err == -ENOTCONN);
      } else if (err == -EMSGSIZE) {
        restart_message(s);
      } else {
        chunk_failed(s);
      }
    }
  }
}

/* The check before a data message starts a session. False: the session ends here (deferred, or the
 * radio is finding out). */
static bool link_checked(struct uplink_sched *s, const struct uplink_radio *r) {
  uint32_t now = now_ms(s);
  enum uplink_link l = r->link_check();

  if (l == US_LINK_WAIT) {
    /* The evaluation counts against the deferral time: a radio that never answers must not
     * hold the data for ever. When the time is up (or the queue is half full) it counts as OK.
     */
    if (!s->deferring) {
      s->deferring = true;
      s->defer_since_ms = now;
    }
    if (us_defer_ok(false, s->defer_max_ms, true, s->defer_since_ms, now, s->q.used, s->q.cap)) {
      s->stats.link_waits++;
      s->recheck = true;
      s->ops->schedule(s->ctx, US_LINK_WAIT_MS);
      end_session(s);
      return false;
    }
    s->stats.link_wait_timeouts++;
    l = US_LINK_OK;
  }
  if (l == US_LINK_BAD &&
      us_defer_ok(false, s->defer_max_ms, s->deferring, s->defer_since_ms, now, s->q.used, s->q.cap)) {
    uint32_t delay = US_DEFER_RECHECK_MS;

    if (!s->deferring) {
      s->deferring = true;
      s->defer_since_ms = now;
    }
    uint32_t left = s->defer_max_ms - (uint32_t)(now - s->defer_since_ms);

    if (left + 1 < delay) {
      delay = left + 1; /* look again when the time is up */
    }
    s->stats.link_deferred++;
    s->recheck = true;
    s->ops->schedule(s->ctx, delay);
    end_session(s);
    return false;
  }
  s->deferring = false; /* the link is fine, or the wait is over: a new window for the next time */
  s->no_check = true;
  return true;
}

static void begin_next(struct uplink_sched *s) {
  const struct uplink_radio *r;
  struct msg_queue *q;

  if (s->q.count == 0) {
    s->deferring = false; /* nothing waits for a better link (an alert session sent the data) */
  }
  while (s->qa.count > 0 || s->q.count > 0) {
    q = s->qa.count > 0 ? &s->qa : &s->q;
    size_t mtu;

    r = us_select(s);
    if (r == NULL) {
      s->ops->error(s->ctx, US_ERR_RADIO);
      end_session(s);
      return;
    }
    mtu = radio_mtu(r); /* read once: it may change while the message is chunked */
    if (mtu > US_MAX_MTU) {
      s->ops->error(s->ctx, US_ERR_RADIO);
      end_session(s);
      return;
    }
    if (!r->ready()) {
      end_session(s);
      return;
    }
    if (q == &s->qa) {
      s->no_check = true; /* an alert or status opens the connection: the data ride along */
    }
    if (!s->no_check && r->link_check != NULL && s->defer_max_ms != 0 && !link_checked(s, r)) {
      return;
    }
    s->cur_len = mq_peek(q, s->cur, US_MAX_MSG);
    if (s->cur_len && r->prepare && mq_peek_flag(q) && s->ops->prepare != NULL) {
      /* A plain message on a radio that asks for it (the node: Uplink.node_id on plain LTE): the app changes
       * it now, for the radio it is sent on. */
      s->cur_len = s->ops->prepare(s->ctx, s->cur, s->cur_len, sizeof(s->cur), r);
      if (s->cur_len > sizeof(s->cur)) {
        s->cur_len = 0; /* a length over the buffer is a failed prepare: drop the message below */
      }
    }
    s->chunk_cnt = s->cur_len ? sensei_frag_count(s->cur_len, mtu) : 0;
    if (s->chunk_cnt == 0) {
      /* Does not fit this radio (more than 255 chunks): drop it. */
      pop_head(s, q, false);
      if (s->fail_q == q) {
        s->head_fails = 0;
      }
      s->stats.msgs_dropped++;
      s->ops->error(s->ctx, US_ERR_RADIO);
      continue;
    }
    s->chunk_idx = 0;
    if (s->resume) {
      /* After a deferral in the middle of a message: go on with the refused chunk only for
       * the same message, chunked the same way, on a radio whose collector keeps the chunks
       * (LoRa), and soon enough. Else a new message id and chunk 0, so the collector never
       * mixes two sets (or waits for chunks it will never get). */
      bool same = s->cur_q == q && r == s->resume_radio && s->msg_id == s->resume_msg_id &&
                  s->cur_len == s->resume_len && msg_hash(s->cur, s->cur_len) == s->resume_hash;
      bool recent = (uint32_t)(now_ms(s) - s->resume_ms) < US_RESUME_MAX_MS;

      if (same && r->keeps_reasm && recent && s->chunk_cnt == s->resume_cnt && mtu == s->resume_mtu) {
        s->chunk_idx = s->resume_idx;
      } else {
        s->msg_id++;
        s->stats.restarted++;
      }
      s->resume = false;
    }
    s->cur_q = q;
    s->cur_radio = r;
    s->cur_mtu = mtu;
    if (s->conn_defers == 0) {
      s->attempts = 0; /* kept while the shield restarts under the same message */
    }
    s->unconf_retried = false;
    send_chunk(s);
    return;
  }
  s->deferring = false;
  end_session(s);
}

void us_start(struct uplink_sched *s) {
  if (s->state != US_IDLE) {
    return;
  }
  begin_next(s);
}

void us_run(struct uplink_sched *s) {
  if (s->state == US_IDLE) {
    if (s->recheck) {
      s->recheck = false;
      begin_next(s); /* after a deferral or a link check: ask the radio again */
    }
  } else if (s->state == US_BACKOFF) {
    send_chunk(s);
  } else if (s->state == US_SENDING) {
    int32_t left = (int32_t)(s->deadline_ms - now_ms(s));

    if (left > 0) {
      s->ops->schedule(s->ctx, (uint32_t)left); /* an earlier request, not the timeout */
      return;
    }
    s->stats.timeouts++;
    s->send_id++; /* a late completion of this send is ignored */
    chunk_failed(s);
  }
}

bool us_stalled(const struct uplink_sched *s, uint32_t now, uint32_t stall_ms) {
  return s->state != US_IDLE && (uint32_t)(now - s->progress_ms) > stall_ms;
}

void us_abort(struct uplink_sched *s) {
  if (s->resume) {
    s->msg_id++; /* the collector may hold the chunks sent so far: do not reuse their id */
  }
  s->resume = false;
  s->deferring = false;
  s->recheck = false;
  if (s->state != US_IDLE) {
    end_session(s);
  }
}

uint32_t us_node_offset_s(uint64_t node_id, uint32_t period_s) {
  uint64_t h = node_id; /* the 64-bit finalizer of MurmurHash3 */

  if (period_s == 0) {
    return 0;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return (uint32_t)(h % period_s);
}

uint32_t us_next_tick_ms(bool unix_valid, uint32_t unix_s, int64_t uptime_ms, uint32_t period_s, uint32_t offset_s) {
  if (period_s == 0) {
    return 0;
  }
  if (unix_valid) {
    uint32_t pos = (unix_s % period_s + period_s - offset_s % period_s) % period_s;

    return (period_s - pos) * 1000u;
  }
  uint64_t p = (uint64_t)period_s * 1000u;

  return (uint32_t)(p - ((uint64_t)uptime_ms % p));
}
