/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Uplink scheduler with fake radios and a fake clock: fragmentation per MTU, completion-driven
 * sending, retries, radio fallback, queue limits, tick alignment. */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "sensei_link/frag.h"
#include "sensei_uplink/uplink_sched.h"

/* ---- fake clock and ops ---- */

static uint32_t clock_ms;
static uint32_t sched_delay; /* last requested delay, 0 if none */
static int n_sched;
static int err_count[2];

static uint32_t op_now(void *c) {
  (void)c;
  return clock_ms;
}
static void op_schedule(void *c, uint32_t d) {
  (void)c;
  sched_delay = d;
  n_sched++;
}
static void op_error(void *c, enum us_error e) {
  (void)c;
  err_count[e]++;
}
static uint32_t rand_value;
static uint32_t op_rand(void *c) {
  (void)c;
  return rand_value;
}
static int n_end_report, n_end_dropped;
static void op_msg_end(void *c, uint8_t flags, bool delivered) {
  (void)c;
  CHECK(flags & MQ_FLAG_REPORT);
  if (delivered) {
    n_end_report++;
  } else {
    n_end_dropped++;
  }
}
static const struct us_ops ops = {op_now, op_schedule, op_error, op_rand, op_msg_end, NULL};

/* ---- fake radios ---- */

struct fake {
  bool ready;
  int fail_sync; /* next sends refused now (-EBUSY, or sync_err) */
  int sync_err;
  int fail_async; /* next sends complete with an error */
  int fail_err;   /* the error of those (default -EIO) */
  int sends;
  void (*done)(int, void *);
  void *user;
  uint8_t chunks[300][1024];
  size_t lens[300];
  int n;
};

static struct fake fk[3];

static int fake_send(struct fake *f, const uint8_t *c, size_t len, void (*done)(int, void *), void *user) {
  f->sends++;
  if (f->fail_sync > 0) {
    f->fail_sync--;
    return f->sync_err ? f->sync_err : -EBUSY;
  }
  if (f->n < 300) {
    memcpy(f->chunks[f->n], c, len);
    f->lens[f->n++] = len;
  }
  f->done = done;
  f->user = user;
  return 0;
}

/* Complete the chunk in flight. Returns false if none. */
static bool complete(struct fake *f) {
  void (*d)(int, void *) = f->done;
  int err = 0;

  if (!d) {
    return false;
  }
  f->done = NULL;
  if (f->fail_async > 0) {
    f->fail_async--;
    err = f->fail_err ? f->fail_err : -EIO;
  }
  d(err, f->user);
  return true;
}

#define FAKE(i)                                                                                                        \
  static bool ready##i(void) { return fk[i].ready; }                                                                   \
  static int send##i(const uint8_t *c, size_t l, void (*d)(int, void *), void *u) {                                    \
    return fake_send(&fk[i], c, l, d, u);                                                                              \
  }
// clang-format off
FAKE(0)
FAKE(1)
FAKE(2)
// clang-format on

static const struct uplink_radio r_ble = {.id = sensei_Radio_RADIO_BLE, .mtu = 244, .ready = ready0, .send = send0};
static const struct uplink_radio r_lora = {
    .id = sensei_Radio_RADIO_LORA, .mtu = 222, .ready = ready1, .send = send1, .keeps_reasm = true};
static const struct uplink_radio r_lte = {.id = sensei_Radio_RADIO_LTEM, .mtu = 1024, .ready = ready2, .send = send2};

static uint8_t qbuf[16384];
static uint8_t abuf[2048];
static struct uplink_sched s;

static void reset(void) {
  memset(fk, 0, sizeof(fk));
  memset(err_count, 0, sizeof(err_count));
  n_end_report = n_end_dropped = 0;
  sched_delay = 0;
  n_sched = 0;
  clock_ms = 1000;
  rand_value = 0;
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops, NULL);
  for (int i = 0; i < 3; i++) {
    fk[i].ready = true;
  }
}

static void make_msg(uint8_t *m, size_t len, uint8_t seed) {
  for (size_t i = 0; i < len; i++) {
    m[i] = (uint8_t)(seed + i * 7);
  }
}

/* Reassemble what a fake radio saw and compare with the message. */
static bool check_msg(struct fake *f, int first, int count, const uint8_t *msg, size_t len, size_t mtu) {
  uint8_t buf[4096];
  sensei_reasm_t r;
  const uint8_t *out = NULL;
  size_t olen = 0;
  sensei_reasm_status_t st = SENSEI_REASM_INCOMPLETE;

  sensei_reasm_init(&r, buf, sizeof(buf), mtu, 1000);
  for (int i = 0; i < count; i++) {
    st = sensei_reasm_feed(&r, f->chunks[first + i], f->lens[first + i], 0, &out, &olen);
  }
  return st == SENSEI_REASM_COMPLETE && olen == len && memcmp(out, msg, len) == 0;
}

/* ---- tests ---- */

static void test_fragmentation(void) {
  struct {
    const struct uplink_radio *r;
    sensei_Radio id;
    int idx;
    size_t mtu;
  } cases[] = {
      {&r_ble, sensei_Radio_RADIO_BLE, 0, 244},
      {&r_lora, sensei_Radio_RADIO_LORA, 1, 222},
      {&r_lte, sensei_Radio_RADIO_LTEM, 2, 1024},
  };
  static const size_t sizes[] = {1, 50, 219, 220, 241, 242, 500, 2086};
  uint8_t msg[2086];

  for (size_t c = 0; c < 3; c++) {
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
      size_t len = sizes[k];
      size_t expect = sensei_frag_count(len, cases[c].mtu);
      struct fake *f;

      reset();
      us_register(&s, cases[c].r);
      us_set_config(&s, cases[c].id, 0);
      f = &fk[cases[c].idx];
      make_msg(msg, len, (uint8_t)k);
      CHECK(us_submit(&s, msg, len, false) == 0);
      us_start(&s);
      /* Completion driven: one chunk in flight at a time. */
      int guard = 0;
      while (complete(f) && ++guard < 1000) {
      }
      CHECK(f->n == (int)expect);
      CHECK(s.q.count == 0 && s.state == US_IDLE);
      CHECK(s.stats.msgs_sent == 1);
      for (int i = 0; i < f->n; i++) {
        CHECK(f->lens[i] <= cases[c].mtu);
      }
      CHECK(check_msg(f, 0, f->n, msg, len, cases[c].mtu));
    }
  }
}

static void test_one_chunk_at_a_time(void) {
  uint8_t msg[600];

  reset();
  us_register(&s, &r_ble);
  make_msg(msg, sizeof(msg), 1);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(fk[0].sends == 1);
  us_start(&s); /* already sending: no second chunk */
  CHECK(fk[0].sends == 1);
  complete(&fk[0]);
  CHECK(fk[0].sends == 2);
}

/* The radio's MTU may change (LoRa data rate, ADR) while a message is chunked: the chunks keep the
 * MTU of the message start, the next message uses the new one. */
static void test_mtu_fixed_per_message(void) {
  static struct uplink_radio r_var = {.id = sensei_Radio_RADIO_LORA, .mtu = 222, .ready = ready1, .send = send1};
  uint8_t a[500], b[500];
  struct fake *f = &fk[1];
  int na, nb;

  reset();
  r_var.mtu = 222;
  us_register(&s, &r_var);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  make_msg(a, sizeof(a), 3);
  make_msg(b, sizeof(b), 4);
  us_submit(&s, a, sizeof(a), false);
  us_submit(&s, b, sizeof(b), false);
  us_start(&s);
  CHECK(f->n == 1);
  r_var.mtu = 51; /* a settings change in the middle of message a */
  while (complete(f)) {
  }
  na = (int)sensei_frag_count(sizeof(a), 222);
  nb = (int)sensei_frag_count(sizeof(b), 51);
  CHECK(s.stats.msgs_sent == 2);
  CHECK(f->n == na + nb);
  CHECK(check_msg(f, 0, na, a, sizeof(a), 222));
  CHECK(check_msg(f, na, nb, b, sizeof(b), 51));
}

/* A prepare hook of the test: a 9 byte field in front of the message, as the node does for Uplink.node_id. */
static uint64_t prep_id;
static int n_prepare;
static size_t op_prepare(void *c, uint8_t *buf, size_t len, size_t cap, const struct uplink_radio *r) {
  (void)c;
  (void)r;
  n_prepare++;
  if (len + 9 > cap) {
    return 0;
  }
  memmove(buf + 9, buf, len);
  buf[0] = 0x79;
  for (unsigned i = 0; i < 8; i++) {
    buf[1 + i] = (uint8_t)(prep_id >> (8 * i));
  }
  return len + 9;
}
static size_t op_prepare_fail(void *c, uint8_t *buf, size_t len, size_t cap, const struct uplink_radio *r) {
  (void)c;
  (void)buf;
  (void)len;
  (void)cap;
  (void)r;
  return 0;
}
/* A hook that reports a length over the buffer (cap + 1): as a failed prepare. */
static size_t op_prepare_over(void *c, uint8_t *buf, size_t len, size_t cap, const struct uplink_radio *r) {
  (void)c;
  (void)buf;
  (void)len;
  (void)r;
  return cap + 1;
}
static const struct us_ops ops_prep_over = {op_now, op_schedule, op_error, op_rand, op_msg_end, op_prepare_over};
static const struct us_ops ops_prep_fail = {op_now, op_schedule, op_error, op_rand, op_msg_end, op_prepare_fail};
static const struct us_ops ops_prep = {op_now, op_schedule, op_error, op_rand, op_msg_end, op_prepare};

/* The prepare hook runs when the message goes out, for the radio it goes out on: a message queued under another radio,
 * or sealed (not plain), or sent on a radio without prepare, is not changed. */
static void test_prepare_at_send_time(void) {
  static const struct uplink_radio r_tag = {
      .id = sensei_Radio_RADIO_LTEM, .mtu = 1024, .ready = ready2, .send = send2, .prepare = true};
  uint8_t msg[300], tagged[300 + 9];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 4);
  prep_id = 0x1122334455667788ull;
  tagged[0] = 0x79;
  for (unsigned i = 0; i < 8; i++) {
    tagged[1 + i] = (uint8_t)(prep_id >> (8 * i));
  }
  memcpy(tagged + 9, msg, sizeof(msg));

  /* queued while LoRa was set, sent after a switch to LTE: prepared */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep, NULL);
  n_prepare = 0;
  us_register(&s, &r_lora);
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  fk[1].ready = false;
  us_submit_plain(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(fk[1].n == 0 && s.q.count == 1 && n_prepare == 0);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  us_start(&s);
  CHECK(n_prepare == 1);
  CHECK(check_msg(f, 0, f->n, tagged, sizeof(tagged), 1024));
  while (complete(f)) {
  }
  CHECK(s.stats.msgs_sent == 1 && s.q.count == 0);

  /* queued while LTE was set, sent on LoRa: not prepared */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep, NULL);
  n_prepare = 0;
  us_register(&s, &r_lora);
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  fk[2].ready = false;
  us_submit_plain(&s, msg, sizeof(msg), false);
  us_start(&s);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  us_start(&s);
  while (complete(&fk[1])) {
  }
  CHECK(check_msg(&fk[1], 0, fk[1].n, msg, sizeof(msg), 222));
  CHECK(n_prepare == 0);

  /* a sealed (not plain) message on LTE: not prepared */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep, NULL);
  n_prepare = 0;
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(check_msg(f, 0, f->n, msg, sizeof(msg), 1024));
  CHECK(n_prepare == 0);

  /* a plain message without a hook (ops without prepare): unchanged */
  reset();
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  us_submit_plain(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(check_msg(f, 0, f->n, msg, sizeof(msg), 1024));

  /* the largest message still gets its field (US_PREPARE_ROOM); the flag survives the ring wrap and the length */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep, NULL);
  us_register(&s, &r_tag);
  prep_id = 7;
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  {
    static uint8_t big[US_MAX_MSG];

    make_msg(big, sizeof(big), 1);
    CHECK(us_submit_plain(&s, big, sizeof(big), false) == 0);
    CHECK(mq_peek_flag(&s.q) && mq_peek_len(&s.q) == US_MAX_MSG);
    us_start(&s);
    CHECK(s.cur_len == US_MAX_MSG + 9 && s.cur[0] == 0x79 && s.cur[1] == 7);
  }

  /* a hook that cannot do it (returns 0): the message is dropped and the next one goes */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep_fail, NULL);
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  CHECK(us_submit_plain(&s, msg, sizeof(msg), false) == 0);
  CHECK(us_submit(&s, msg, 10, false) == 0);
  us_start(&s);
  CHECK(s.stats.msgs_dropped == 1 && err_count[US_ERR_RADIO] == 1 && f->n == 1 && s.q.count == 1);

  /* a hook that returns more than the buffer holds (cap + 1): the same, nothing is sent from the over-long length */
  reset();
  us_init(&s, qbuf, sizeof(qbuf), abuf, sizeof(abuf), &ops_prep_over, NULL);
  us_register(&s, &r_tag);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  CHECK(us_submit_plain(&s, msg, sizeof(msg), false) == 0);
  CHECK(us_submit(&s, msg, 10, false) == 0);
  us_start(&s);
  CHECK(s.stats.msgs_dropped == 1 && err_count[US_ERR_RADIO] == 1 && f->n == 1 && s.q.count == 1);
  CHECK(s.cur_len == 10);
}

static void test_multiple_messages(void) {
  uint8_t a[300], b[100];

  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  make_msg(a, sizeof(a), 3);
  make_msg(b, sizeof(b), 9);
  us_submit(&s, a, sizeof(a), false);
  us_submit(&s, b, sizeof(b), false);
  us_start(&s);
  while (complete(&fk[1])) {
  }
  CHECK(s.stats.msgs_sent == 2 && s.q.count == 0);
  CHECK(check_msg(&fk[1], 0, 2, a, sizeof(a), 222));
  CHECK(check_msg(&fk[1], 2, 1, b, sizeof(b), 222));
  /* Different message ids. */
  CHECK(fk[1].chunks[0][0] != fk[1].chunks[2][0]);
}

static void test_retries(void) {
  uint8_t msg[400];

  /* Two failures, then success, with retries = 2: backoff 2 s, then 4 s. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  make_msg(msg, sizeof(msg), 5);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  fk[1].fail_async = 2;
  complete(&fk[1]); /* chunk 0 fails */
  CHECK(s.state == US_BACKOFF && sched_delay == US_RETRY_BASE_MS);
  us_run(&s);
  complete(&fk[1]); /* fails again */
  CHECK(sched_delay == 2 * US_RETRY_BASE_MS);
  us_run(&s);
  while (complete(&fk[1])) {
  }
  CHECK(s.stats.retries == 2 && s.stats.msgs_sent == 1 && s.q.count == 0);
  CHECK(err_count[US_ERR_RADIO] == 0);

  /* Refused synchronously (no done callback) is a failure too. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 1);
  us_submit(&s, msg, 100, false);
  fk[1].fail_sync = 1;
  us_start(&s);
  CHECK(s.state == US_BACKOFF && sched_delay == US_RETRY_BASE_MS);
  us_run(&s);
  while (complete(&fk[1])) {
  }
  CHECK(s.stats.msgs_sent == 1);

  /* Retries exhausted: session ends, message stays, error counted. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 1);
  us_submit(&s, msg, 100, false);
  fk[1].fail_async = 100;
  us_start(&s);
  complete(&fk[1]);
  us_run(&s);
  complete(&fk[1]);
  CHECK(s.state == US_IDLE && s.q.count == 1);
  CHECK(err_count[US_ERR_RADIO] == 1);
  /* The next uplink time tries again; after 3 failed sessions the message is dropped. */
  us_start(&s);
  complete(&fk[1]);
  us_run(&s);
  complete(&fk[1]);
  CHECK(s.q.count == 1);
  us_start(&s);
  complete(&fk[1]);
  us_run(&s);
  complete(&fk[1]);
  CHECK(s.q.count == 0 && s.stats.msgs_dropped == 1 && err_count[US_ERR_RADIO] == 3);

  /* Backoff is capped. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 200);
  us_submit(&s, msg, 100, false);
  fk[1].fail_async = 100;
  us_start(&s);
  for (int i = 0; i < 12; i++) {
    complete(&fk[1]);
    us_run(&s);
  }
  complete(&fk[1]);
  CHECK(sched_delay == US_RETRY_MAX_MS);
}

static void test_fallback(void) {
  uint8_t msg[100];

  make_msg(msg, sizeof(msg), 2);

  /* Selected radio missing: BLE is used. */
  reset();
  us_register(&s, &r_ble);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  CHECK(us_select(&s) == &r_ble);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  while (complete(&fk[0])) {
  }
  CHECK(fk[0].n == 1 && s.stats.msgs_sent == 1);

  /* Selected radio present: used. */
  reset();
  us_register(&s, &r_ble);
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  CHECK(us_select(&s) == &r_lora);

  /* Neither: keep queuing, count the error. */
  reset();
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  CHECK(us_select(&s) == NULL);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(s.q.count == 1 && err_count[US_ERR_RADIO] == 1 && s.state == US_IDLE);
  us_register(&s, &r_lte);
  us_start(&s);
  while (complete(&fk[2])) {
  }
  CHECK(s.q.count == 0);

  /* Not ready: nothing sent, message kept, no error. */
  reset();
  us_register(&s, &r_ble);
  fk[0].ready = false;
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(fk[0].sends == 0 && s.q.count == 1 && err_count[US_ERR_RADIO] == 0);
  fk[0].ready = true;
  us_start(&s);
  CHECK(fk[0].sends == 1);

  /* Radio table full. */
  reset();
  for (int i = 0; i < US_MAX_RADIOS; i++) {
    CHECK(us_register(&s, &r_ble) == 0);
  }
  CHECK(us_register(&s, &r_ble) == -ENOSPC);
}

static void test_queue_limits(void) {
  static uint8_t small[1000];
  static uint8_t asmall[1000];
  uint8_t msg[300];

  /* Full: the oldest messages are dropped, the newest are kept. */
  reset();
  us_init(&s, small, sizeof(small), asmall, sizeof(asmall), &ops, NULL);
  us_register(&s, &r_ble);
  make_msg(msg, sizeof(msg), 4);
  for (int i = 0; i < 6; i++) {
    msg[0] = (uint8_t)i;
    CHECK(us_submit(&s, msg, sizeof(msg), false) == 0);
  }
  CHECK(s.q.count == 3 && s.stats.msgs_evicted == 3 && err_count[US_ERR_QUEUE_FULL] == 3);
  us_start(&s);
  CHECK(fk[0].chunks[0][3] == 3); /* first message out is number 3 */
  CHECK(us_submit(&s, msg, US_MAX_MSG + 1, false) == -EINVAL);
  CHECK(us_submit(&s, msg, 0, false) == -EINVAL);
}

static void test_priority_queue(void) {
  static uint8_t dbuf[1000];
  static uint8_t alb[700];
  uint8_t msg[300], al[100];
  size_t m, b;

  reset();
  us_init(&s, dbuf, sizeof(dbuf), alb, sizeof(alb), &ops, NULL);
  us_register(&s, &r_ble);
  make_msg(msg, sizeof(msg), 4);
  make_msg(al, sizeof(al), 8);
  for (int i = 0; i < 5; i++) {
    CHECK(us_submit(&s, msg, sizeof(msg), false) == 0);
  }
  /* A full data queue does not take the alert's room. */
  al[0] = 0xA1;
  CHECK(us_submit(&s, al, sizeof(al), true) == 0);
  CHECK(s.qa.count == 1 && s.stats.msgs_evicted == 2);
  us_queued(&s, &m, &b);
  CHECK(m == 4 && b == 3 * 302 + 102);
  /* The alert goes first, then the data. */
  us_start(&s);
  CHECK(fk[0].n == 1 && fk[0].lens[0] == 103 && fk[0].chunks[0][3] == 0xA1);
  while (complete(&fk[0])) {
  }
  CHECK(s.q.count == 0 && s.qa.count == 0 && s.stats.msgs_sent == 4);
  CHECK(fk[0].n == 1 + 3 * 2); /* alert, then three data messages of two chunks */

  /* The alert queue drops its own oldest. */
  reset();
  us_init(&s, dbuf, sizeof(dbuf), alb, sizeof(alb), &ops, NULL);
  for (int i = 0; i < 8; i++) {
    al[0] = (uint8_t)i;
    CHECK(us_submit(&s, al, sizeof(al), true) == 0);
  }
  CHECK(s.qa.count == 6 && s.stats.msgs_evicted == 2 && s.q.count == 0);
  CHECK(us_submit(&s, msg, 700, true) == -ENOMEM); /* does not fit an empty queue */
  CHECK(s.qa.count == 6);

  /* The message being sent is the oldest: it is dropped and the session ends. */
  reset();
  us_init(&s, dbuf, sizeof(dbuf), alb, sizeof(alb), &ops, NULL);
  us_register(&s, &r_ble);
  for (int i = 0; i < 3; i++) {
    msg[0] = (uint8_t)i;
    us_submit(&s, msg, sizeof(msg), false);
  }
  us_start(&s);
  CHECK(s.state == US_SENDING && fk[0].sends == 1);
  msg[0] = 9;
  CHECK(us_submit(&s, msg, sizeof(msg), false) == 0);
  CHECK(s.state == US_IDLE && s.q.count == 3);
  complete(&fk[0]); /* late completion of the dropped message: ignored */
  CHECK(fk[0].sends == 1 && s.stats.msgs_sent == 0);
  us_start(&s);
  CHECK(fk[0].sends == 2 && fk[0].chunks[1][3] == 1); /* the new oldest, from chunk 0 */
}

static void test_too_many_chunks(void) {
  static const struct uplink_radio r_tiny = {.id = sensei_Radio_RADIO_BLE, .mtu = 4, .ready = ready0, .send = send0};
  uint8_t big[300], small[100];

  /* 300 B at 1 B per chunk needs more than 255 chunks: dropped, the next message goes. */
  reset();
  us_register(&s, &r_tiny);
  make_msg(big, sizeof(big), 1);
  make_msg(small, sizeof(small), 2);
  us_submit(&s, big, sizeof(big), false);
  us_submit(&s, small, 10, false);
  us_start(&s);
  CHECK(s.stats.msgs_dropped == 1 && err_count[US_ERR_RADIO] == 1);
  CHECK(fk[0].sends == 1 && fk[0].lens[0] == 4);
  CHECK(fk[0].chunks[0][2] == 10); /* chunk count of the small message */
}

static void test_abort(void) {
  uint8_t msg[600];

  reset();
  us_register(&s, &r_ble);
  make_msg(msg, sizeof(msg), 1);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  us_abort(&s);
  CHECK(s.state == US_IDLE && s.q.count == 1);
  complete(&fk[0]); /* late completion is ignored */
  CHECK(fk[0].sends == 1);
  us_start(&s);
  CHECK(fk[0].sends == 2 && fk[0].chunks[1][1] == 0); /* restarts at chunk 0 */
}

static void test_tick(void) {
  CHECK(us_next_tick_ms(true, 1000, 0, 3600, 0) == 2600u * 1000u);
  CHECK(us_next_tick_ms(true, 3600, 0, 3600, 0) == 3600u * 1000u);
  CHECK(us_next_tick_ms(true, 86399, 5, 86400, 0) == 1000u);
  CHECK(us_next_tick_ms(false, 0, 90500, 60, 0) == 29500u);
  CHECK(us_next_tick_ms(false, 0, 0, 60, 0) == 60000u);
  CHECK(us_next_tick_ms(true, 5, 5, 0, 0) == 0);
  /* Always in (0, period]. */
  for (uint32_t t = 0; t < 200; t++) {
    uint32_t d = us_next_tick_ms(true, t, 0, 60, 0);

    CHECK(d >= 1000 && d <= 60000 && (t + d / 1000) % 60 == 0);
  }
}

static void test_node_offset(void) {
  int used[60] = {0}, n_used = 0;

  /* The tick is offset seconds past a multiple of the period. */
  CHECK(us_next_tick_ms(true, 1000, 0, 3600, 100) == 2700u * 1000u);
  CHECK(us_next_tick_ms(true, 3700, 0, 3600, 100) == 3600u * 1000u);
  CHECK(us_next_tick_ms(true, 3699, 0, 3600, 100) == 1000u);
  /* An offset above the period counts modulo the period. */
  CHECK(us_next_tick_ms(true, 3599, 0, 3600, 3700) == 101u * 1000u);
  for (uint32_t t = 0; t < 300; t++) {
    uint32_t d = us_next_tick_ms(true, t, 0, 60, 17);

    CHECK(d >= 1000 && d <= 60000 && (t + d / 1000) % 60 == 17);
  }
  /* Without time there is no offset. */
  CHECK(us_next_tick_ms(false, 0, 90500, 60, 17) == 29500u);

  /* The offset comes from the id, stays inside the period and spreads the nodes. */
  CHECK(us_node_offset_s(1, 0) == 0);
  CHECK(us_node_offset_s(0x123456789abcdef0ULL, 900) == us_node_offset_s(0x123456789abcdef0ULL, 900));
  for (uint64_t id = 0; id < 600; id++) {
    /* Ids that differ in few bits. */
    uint32_t o = us_node_offset_s(0xF0E1D2C3B4A00000ULL + id, 60);

    CHECK(o < 60);
    if (o < 60 && !used[o]++) {
      n_used++;
    }
  }
  CHECK(n_used >= 55); /* 600 nodes fill nearly all 60 seconds */
}

static void test_timeout(void) {
  uint8_t msg[400];

  make_msg(msg, sizeof(msg), 5);

  /* A chunk the radio never completes: failed after the timeout, then retried. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 1);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(sched_delay == US_CHUNK_TIMEOUT_MS);
  void (*old_done)(int, void *) = fk[1].done;
  void *old_user = fk[1].user;

  clock_ms += US_CHUNK_TIMEOUT_MS - 1000;
  us_run(&s); /* too early (an older request): wait for the rest */
  CHECK(s.state == US_SENDING && sched_delay == 1000 && s.stats.timeouts == 0);
  clock_ms += 1000;
  us_run(&s);
  CHECK(s.state == US_BACKOFF && s.stats.timeouts == 1 && s.stats.retries == 1);
  CHECK(sched_delay == US_RETRY_BASE_MS);
  us_run(&s); /* retry: chunk 0 again */
  CHECK(fk[1].sends == 2 && s.state == US_SENDING);
  old_done(0, old_user); /* the first send completes very late: not this chunk */
  CHECK(fk[1].sends == 2 && s.chunk_idx == 0);
  while (complete(&fk[1])) {
  }
  CHECK(s.stats.msgs_sent == 1 && s.q.count == 0);

  /* No retries left: the session ends, the message stays, an error is counted. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  clock_ms += US_CHUNK_TIMEOUT_MS;
  us_run(&s);
  CHECK(s.state == US_IDLE && s.q.count == 1 && err_count[US_ERR_RADIO] == 1);

  /* A timer that fires with nothing to do changes nothing. */
  us_run(&s);
  CHECK(s.state == US_IDLE);
}

static void test_stale_completion(void) {
  uint8_t msg[1000];

  /* A late completion of an aborted session is not counted for the new one (A6). */
  reset();
  make_msg(msg, sizeof(msg), 0x55);
  us_register(&s, &r_ble);
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s); /* LoRa chunk 0 in flight */
  void (*old_done)(int, void *) = fk[1].done;
  void *old_user = fk[1].user;

  us_set_config(&s, sensei_Radio_RADIO_BLE, 2); /* the radio setting changed */
  us_abort(&s);
  us_start(&s); /* BLE chunk 0 in flight */
  CHECK(fk[0].sends == 1 && s.chunk_idx == 0);
  old_done(0, old_user);
  CHECK(fk[0].sends == 1 && s.chunk_idx == 0 && s.stats.chunks_sent == 0);
  old_done(-EIO, old_user); /* a late failure is ignored too */
  CHECK(s.state == US_SENDING && s.stats.retries == 0);
  complete(&fk[0]);
  CHECK(fk[0].sends == 2 && s.chunk_idx == 1);
}

/* D7: a late completion of a send on another radio must not hit the send that is in flight now,
 * however many sends came in between (the ids are not reused). */
static void test_late_completion_after_a_radio_switch(void) {
  uint8_t msg[2000];

  reset();
  make_msg(msg, sizeof(msg), 0x5A);
  us_register(&s, &r_ble);
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s); /* LoRa chunk 0 in flight */
  void (*old_done)(int, void *) = fk[1].done;
  void *old_user = fk[1].user;

  us_abort(&s);
  us_set_config(&s, sensei_Radio_RADIO_BLE, 3);
  us_start(&s);
  for (int i = 0; i < 8; i++) { /* more sends than a small ring of tokens holds */
    CHECK(complete(&fk[0]));
  }
  size_t idx = s.chunk_idx;

  CHECK(s.state == US_SENDING);
  old_done(-EIO, old_user);
  CHECK(s.chunk_idx == idx && s.stats.retries == 0 && s.state != US_BACKOFF);
  old_done(0, old_user);
  CHECK(s.chunk_idx == idx && s.stats.chunks_sent == 8);
}

/* G1: the chunk size comes from mtu_now() when a message starts; a radio that answers -EMSGSIZE
 * makes the message start again with the size it has then. */
static size_t mtu_value;
static size_t mtu_now_fn(void) { return mtu_value; }
static const struct uplink_radio r_dyn = {.id = sensei_Radio_RADIO_LORA,
                                          .mtu = 222,
                                          .ready = ready1,
                                          .send = send1,
                                          .mtu_now = mtu_now_fn,
                                          .keeps_reasm = true};

static void test_mtu_now_and_restart_on_emsgsize(void) {
  uint8_t msg[200];
  struct fake *f = &fk[1];
  size_t cnt;

  make_msg(msg, sizeof(msg), 9);

  /* the size is read at the start of each message, and never above .mtu */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(s.cur_mtu == 115 && f->lens[0] == 115);
  while (complete(f)) {
  }
  CHECK(check_msg(f, 0, f->n, msg, sizeof(msg), 115) && s.stats.msgs_sent == 1);
  mtu_value = 5000;
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(s.cur_mtu == 222);

  /* chunk 1 does not fit the next uplink: the message starts again with 51 B, as a new message */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && f->n == 2); /* chunk 0 out, chunk 1 in flight */
  uint8_t first_id = f->chunks[0][0];

  mtu_value = 51;
  f->fail_async = 1;
  f->fail_err = -EMSGSIZE;
  CHECK(complete(f));
  cnt = sensei_frag_count(sizeof(msg), 51);
  CHECK(f->n == 3 && f->lens[2] == 51 && s.chunk_idx == 0 && s.chunk_cnt == cnt && s.cur_mtu == 51);
  CHECK(f->chunks[2][0] != first_id && f->chunks[2][1] == 0 && f->chunks[2][2] == cnt);
  CHECK(s.attempts == 1 && s.stats.retries == 1 && s.state == US_SENDING && err_count[US_ERR_RADIO] == 0);
  while (complete(f)) {
  }
  CHECK(check_msg(f, 2, (int)cnt, msg, sizeof(msg), 51));
  CHECK(s.stats.msgs_sent == 1 && s.q.count == 0 && s.state == US_IDLE);

  /* no attempt left: the session fails and the message stays for the next time */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  mtu_value = 51;
  f->fail_async = 1;
  f->fail_err = -EMSGSIZE;
  CHECK(complete(f));
  CHECK(s.state == US_IDLE && s.q.count == 1 && err_count[US_ERR_RADIO] == 1);

  /* a size that makes the message too long for 255 chunks: dropped, the next message goes */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  uint8_t big[300];

  make_msg(big, sizeof(big), 1);
  us_submit(&s, big, sizeof(big), false);
  us_submit(&s, msg, 10, false);
  us_start(&s);
  mtu_value = 4; /* 1 B of payload per chunk: 300 chunks */
  f->fail_async = 1;
  f->fail_err = -EMSGSIZE;
  CHECK(complete(f));
  CHECK(s.stats.msgs_dropped == 1 && err_count[US_ERR_RADIO] == 1 && s.q.count == 1);
  CHECK(f->lens[f->n - 1] <= 4 && s.state == US_SENDING);

  /* -EMSGSIZE and the size did not change (the radio's answer is no use): a failed chunk with the
   * backoff, the same chunk again after it, not at once */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(f->n == 1);
  f->fail_async = 1;
  f->fail_err = -EMSGSIZE;
  CHECK(complete(f));
  CHECK(f->n == 1 && s.state == US_BACKOFF && s.attempts == 1 && s.stats.retries == 1 &&
        sched_delay >= US_RETRY_BASE_MS);
  us_run(&s);
  CHECK(f->n == 2 && f->lens[1] == f->lens[0] && f->chunks[1][0] == f->chunks[0][0] && f->chunks[1][1] == 0);
  /* it keeps failing: the session ends after the retries, the message stays */
  f->fail_async = 5;
  f->fail_err = -EMSGSIZE;
  while (complete(f) && s.state == US_SENDING) {
  }
  while (s.state == US_BACKOFF) {
    us_run(&s);
    complete(f);
  }
  CHECK(s.state == US_IDLE && s.q.count == 1 && f->n == 3 && err_count[US_ERR_RADIO] == 1);
}

/* A radio that defers (-EAGAIN, the LoRa duty cycle): the session ends, the message stays, nothing
 * is counted against it; the next start goes on with the chunk that was refused (the chunks before
 * it are out; starting over each time would never get past a chunk that the duty cycle refuses
 * once). */
static void test_deferral(void) {
  uint8_t msg[300];
  struct fake *f = &fk[1];

  make_msg(msg, sizeof(msg), 6);

  /* from done(), after the first chunk */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && f->n == 2); /* chunk 0 done, chunk 1 in flight */
  f->fail_async = 1;
  f->fail_err = -EAGAIN;
  CHECK(complete(f));
  CHECK(s.state == US_IDLE && s.q.count == 1 && s.head_fails == 0 && s.attempts <= 1 && s.stats.deferred == 1 &&
        s.stats.retries == 0 && s.stats.msgs_dropped == 0 && err_count[US_ERR_RADIO] == 0 && f->n == 2);
  us_start(&s); /* the radio is up again (uplink_flush) */
  CHECK(f->n == 3 && f->chunks[2][1] == 1 && f->chunks[2][0] == f->chunks[0][0]); /* chunk 1 again, same message */
  while (complete(f)) {
  }
  CHECK(s.stats.msgs_sent == 1 && s.q.count == 0);
  CHECK(f->n == 3); /* chunk 0 once, chunk 1 twice (once refused): nothing that went out is sent again */

  /* refused again and again at the same chunk: still on that chunk, the message gets through */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && f->n == 2);
  for (int i = 0; i < 4; i++) {
    f->fail_async = 1;
    f->fail_err = -EAGAIN;
    CHECK(complete(f));
    CHECK(s.state == US_IDLE);
    us_start(&s);
    CHECK(f->chunks[f->n - 1][1] == 1);
  }
  while (complete(f)) {
  }
  CHECK(s.stats.msgs_sent == 1 && s.stats.deferred == 4 && s.q.count == 0);

  /* the head is another message after the deferral (the oldest was evicted, here: the sender
   * dropped it by an abort): start from chunk 0 */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && f->n == 2);
  f->fail_async = 1;
  f->fail_err = -EAGAIN;
  CHECK(complete(f));
  us_abort(&s);
  us_start(&s);
  CHECK(f->chunks[f->n - 1][1] == 0);

  /* the chunk size changed meanwhile: another chunking, another message id, from chunk 0 */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && f->n == 2);
  f->fail_async = 1;
  f->fail_err = -EAGAIN;
  CHECK(complete(f));
  mtu_value = 51;
  us_start(&s);
  CHECK(f->chunks[f->n - 1][1] == 0 && f->chunks[f->n - 1][0] != f->chunks[0][0]);

  /* from send() itself, again and again: it never costs the message */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 1);
  us_submit(&s, msg, sizeof(msg), false);
  f->sync_err = -EAGAIN;
  f->fail_sync = 10;
  for (int i = 0; i < 6; i++) {
    us_start(&s);
    CHECK(s.state == US_IDLE && s.q.count == 1);
  }
  CHECK(s.stats.deferred == 6 && s.head_fails == 0 && s.stats.msgs_dropped == 0 && s.stats.retries == 0 &&
        err_count[US_ERR_RADIO] == 0);
  f->fail_sync = 0;
  us_start(&s);
  while (complete(f)) {
  }
  CHECK(s.stats.msgs_sent == 1);

  /* -EMSGSIZE from send(): restart if the size changed, else a failed chunk with backoff */
  reset();
  mtu_value = 115;
  us_register(&s, &r_dyn);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 2);
  us_submit(&s, msg, sizeof(msg), false);
  f->sync_err = -EMSGSIZE;
  f->fail_sync = 1;
  us_start(&s);
  CHECK(f->n == 0 && s.state == US_BACKOFF && s.stats.retries == 1);
  us_run(&s);
  CHECK(f->n == 1 && f->lens[0] == 115);
}

/* A deferred message goes on at the refused chunk only if the radio keeps the collector's partial
 * message (LoRa) and the last chunk is recent. Else: chunk 0 with a new message id (LTE: the next
 * connection comes from another address, the chunks before it would never be seen). */
static void test_deferral_resume_rules(void) {
  uint8_t lte_msg[2100], lora_msg[300];
  struct fake *fl = &fk[1], *ft = &fk[2];
  uint8_t id;

  make_msg(lte_msg, sizeof(lte_msg), 3);
  make_msg(lora_msg, sizeof(lora_msg), 8);

  /* LTE, -EAGAIN (shield update) or -ENOTCONN (shield restart) in the middle: chunk 0, new id */
  for (int k = 0; k < 2; k++) {
    reset();
    us_register(&s, &r_lte);
    us_set_config(&s, sensei_Radio_RADIO_LTEM, 3);
    us_submit(&s, lte_msg, sizeof(lte_msg), false); /* 3 chunks of 1024 B */
    us_start(&s);
    CHECK(complete(ft) && ft->n == 2); /* chunk 0 done, chunk 1 in flight */
    id = ft->chunks[0][0];
    ft->fail_async = 1;
    ft->fail_err = k ? -ENOTCONN : -EAGAIN;
    CHECK(complete(ft) && s.state == US_IDLE && s.q.count == 1);
    us_start(&s);
    CHECK(ft->n == 3 && ft->chunks[2][1] == 0 && ft->chunks[2][0] != id && s.stats.restarted == 1);
    while (complete(ft)) {
    }
    CHECK(s.stats.msgs_sent == 1 && ft->n == 5 && s.q.count == 0);
    CHECK(check_msg(ft, 2, 3, lte_msg, sizeof(lte_msg), 1024)); /* the new set is complete */
  }

  /* LoRa, the last chunk is older than the limit: chunk 0, new id */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, lora_msg, sizeof(lora_msg), false);
  us_start(&s);
  CHECK(complete(fl) && fl->n == 2);
  id = fl->chunks[0][0];
  fl->fail_async = 1;
  fl->fail_err = -EAGAIN;
  CHECK(complete(fl));
  clock_ms += US_RESUME_MAX_MS; /* the shield update or a slow rejoin took this long */
  us_start(&s);
  CHECK(fl->n == 3 && fl->chunks[2][1] == 0 && fl->chunks[2][0] != id && s.stats.restarted == 1);
  while (complete(fl)) {
  }
  CHECK(s.stats.msgs_sent == 1 && check_msg(fl, 2, 2, lora_msg, sizeof(lora_msg), 222));

  /* LoRa within the limit (one ms below it): resume at the refused chunk, same id */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, lora_msg, sizeof(lora_msg), false);
  us_start(&s);
  CHECK(complete(fl) && fl->n == 2);
  id = fl->chunks[0][0];
  fl->fail_async = 1;
  fl->fail_err = -EAGAIN;
  CHECK(complete(fl));
  clock_ms += US_RESUME_MAX_MS - 1;
  us_start(&s);
  CHECK(fl->n == 3 && fl->chunks[2][1] == 1 && fl->chunks[2][0] == id && s.stats.restarted == 0);
  while (complete(fl)) {
  }
  CHECK(s.stats.msgs_sent == 1);

  /* the age counts from the last chunk that went out, not from the first refusal: refused
   * again and again, the message is too old to resume at the next try */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, lora_msg, sizeof(lora_msg), false);
  us_start(&s);
  CHECK(complete(fl) && fl->n == 2);
  id = fl->chunks[0][0];
  for (int i = 0; i < 2; i++) {
    fl->fail_async = 1;
    fl->fail_err = -EAGAIN;
    CHECK(complete(fl));
    clock_ms += US_RESUME_MAX_MS / 2 + 1000;
    us_start(&s);
  }
  CHECK(fl->chunks[fl->n - 1][1] == 0 && fl->chunks[fl->n - 1][0] != id);

  /* another message is the head after the deferral: chunk 0 and also a new id */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, lora_msg, sizeof(lora_msg), false);
  us_start(&s);
  CHECK(complete(fl) && fl->n == 2);
  id = fl->chunks[0][0];
  fl->fail_async = 1;
  fl->fail_err = -EAGAIN;
  CHECK(complete(fl));
  us_abort(&s);
  us_start(&s);
  CHECK(fl->chunks[fl->n - 1][1] == 0 && fl->chunks[fl->n - 1][0] != id);
}

/* A shield that restarts on every send (-ENOTCONN, then a connect): counted per head message
 * without progress; the attempts are kept; after US_MAX_CONN_DEFERS a failed session; the message
 * is dropped after the usual number of failed sessions. The duty cycle (-EAGAIN) is never counted.
 */
static void test_not_connected_deferral(void) {
  uint8_t msg[100], big[2100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 5);
  make_msg(big, sizeof(big), 5);

  /* timeout, restart (retry refused with -ENOTCONN), timeout, restart ...: ends in a drop */
  reset();
  us_register(&s, &r_lte);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 3);
  us_submit(&s, msg, sizeof(msg), false);
  for (int cycle = 1; cycle <= 5; cycle++) {
    us_start(&s); /* the radio is up again (uplink_flush) */
    CHECK(s.state == US_SENDING);
    if (cycle == 2 || cycle == 3) {
      CHECK(s.attempts == cycle - 1); /* kept, not reset by the new session */
    }
    clock_ms += US_CHUNK_TIMEOUT_MS;
    us_run(&s); /* no answer: timeout, retry later */
    CHECK(s.state == US_BACKOFF);
    f->sync_err = -ENOTCONN; /* the shield restarted */
    f->fail_sync = 1;
    clock_ms += sched_delay;
    us_run(&s);
    CHECK(s.state == US_IDLE);
    CHECK(s.stats.conn_deferred == (uint32_t)cycle);
    if (cycle < 3) {
      CHECK(s.head_fails == 0 && s.q.count == 1 && err_count[US_ERR_RADIO] == 0);
    } else if (cycle < 5) {
      CHECK(s.head_fails == cycle - 2 && s.q.count == 1); /* failed sessions now */
    }
  }
  CHECK(s.q.count == 0 && s.stats.msgs_dropped == 1 && s.head_fails == 0 && s.stats.msgs_sent == 0);

  /* the same, the shield restart comes with the completion (done(-ENOTCONN)) */
  reset();
  us_register(&s, &r_lte);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 3);
  us_submit(&s, msg, sizeof(msg), false);
  for (int cycle = 1; cycle <= 5; cycle++) {
    us_start(&s);
    f->fail_async = 1;
    f->fail_err = -ENOTCONN;
    CHECK(complete(f) && s.state == US_IDLE);
  }
  CHECK(s.q.count == 0 && s.stats.msgs_dropped == 1 && s.stats.msgs_sent == 0);

  /* a chunk that goes through is progress: the count starts again */
  reset();
  us_register(&s, &r_lte);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 3);
  us_submit(&s, big, sizeof(big), false); /* 3 chunks */
  for (int i = 0; i < 2; i++) {
    us_start(&s);
    f->fail_async = 1;
    f->fail_err = -ENOTCONN;
    CHECK(complete(f) && s.conn_defers == i + 1);
  }
  us_start(&s);
  CHECK(complete(f) && s.conn_defers == 0 && s.chunk_idx == 1); /* chunk 0 out */
  for (int i = 0; i < 2; i++) {
    f->fail_async = 1;
    f->fail_err = -ENOTCONN;
    CHECK(complete(f) && s.state == US_IDLE && s.head_fails == 0);
    us_start(&s);
  }
  while (complete(f)) {
  }
  CHECK(s.stats.msgs_sent == 1 && s.stats.msgs_dropped == 0 && err_count[US_ERR_RADIO] == 0);

  /* the duty cycle of LoRa (-EAGAIN) is not counted, however often it comes */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 3);
  us_submit(&s, msg, sizeof(msg), false);
  f = &fk[1];
  for (int i = 0; i < 20; i++) {
    us_start(&s);
    f->fail_async = 1;
    f->fail_err = -EAGAIN;
    CHECK(complete(f) && s.state == US_IDLE);
  }
  CHECK(s.q.count == 1 && s.head_fails == 0 && s.stats.msgs_dropped == 0 && s.stats.conn_deferred == 0 &&
        s.stats.deferred == 20 && err_count[US_ERR_RADIO] == 0);
  us_start(&s);
  CHECK(complete(f) && s.stats.msgs_sent == 1);
}

/* ---- LTE: the last chunk is marked, the answer of the collector, deferral on a bad link ---- */

static bool last_flags[300];
static int link_state; /* what link_check() answers (enum uplink_link) */
static int n_link_checks;

static int send_last2(const uint8_t *c, size_t l, bool last, void (*d)(int, void *), void *u) {
  if (fk[2].n < 300) {
    last_flags[fk[2].n] = last;
  }
  return fake_send(&fk[2], c, l, d, u);
}
static enum uplink_link check2(void) {
  n_link_checks++;
  return (enum uplink_link)link_state;
}
static const struct uplink_radio r_lte_ra = {.id = sensei_Radio_RADIO_LTEM,
                                             .mtu = 1024,
                                             .ready = ready2,
                                             .send = send2,
                                             .send_last = send_last2,
                                             .link_check = check2};

/* The radio is told which chunk is the last of the session: the last chunk of the last queued
 * message. A radio without send_last gets send(). */
static void test_last_chunk(void) {
  uint8_t msg[2100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 4);
  reset();
  memset(last_flags, 0, sizeof(last_flags));
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false); /* 3 chunks */
  us_submit(&s, msg, 100, false);         /* 1 chunk */
  us_start(&s);
  while (complete(f)) {
  }
  CHECK(f->n == 4 && s.stats.msgs_sent == 2);
  CHECK(!last_flags[0] && !last_flags[1] && !last_flags[2] && last_flags[3]);

  /* one message: its last chunk */
  reset();
  memset(last_flags, 0, sizeof(last_flags));
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  while (complete(f)) {
  }
  CHECK(f->n == 3 && !last_flags[0] && !last_flags[1] && last_flags[2]);

  /* an alert and data: the alert is not the last of the session, the data message is. Data
   * that is queued while the alert goes out makes the alert's chunk a non-last one too. */
  reset();
  memset(last_flags, 0, sizeof(last_flags));
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, 100, false);
  us_submit(&s, msg, 50, true);
  us_start(&s);
  while (complete(f)) {
  }
  CHECK(f->n == 2 && f->lens[0] == 53 && !last_flags[0] && last_flags[1]);

  /* a radio without send_last works as before */
  reset();
  us_register(&s, &r_lte);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  while (complete(f)) {
  }
  CHECK(f->n == 3 && s.stats.msgs_sent == 1);
}

/* A chunk that did not go out (an error below 0) is a failed chunk: retried with the backoff, the
 * message stays for the next session when the retries are used up. */
static void test_no_answer(void) {
  uint8_t msg[2100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 5);
  reset();
  memset(last_flags, 0, sizeof(last_flags));
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(complete(f) && complete(f)); /* chunks 0 and 1 */
  f->fail_async = 1;                 /* the last chunk: sent, no answer */
  f->fail_err = -EIO;
  CHECK(complete(f));
  CHECK(s.state == US_BACKOFF && s.stats.retries == 1 && s.q.count == 1 && f->n == 3);
  clock_ms += 5000;
  us_run(&s);
  CHECK(f->n == 4 && f->chunks[3][1] == 2 && last_flags[3]); /* the same chunk again, still marked last */
  CHECK(complete(f));                                        /* now the answer came */
  CHECK(s.stats.msgs_sent == 1 && s.q.count == 0 && s.state == US_IDLE);

  /* the answer never comes: the retries are used up, the message stays for the next session */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 1);
  us_submit(&s, msg, 100, false);
  f->fail_async = 10;
  f->fail_err = -EIO;
  us_start(&s);
  CHECK(complete(f)); /* attempt 1 */
  clock_ms += 5000;
  us_run(&s);
  CHECK(complete(f)); /* retry */
  CHECK(s.state == US_IDLE && s.q.count == 1 && s.head_fails == 1 && s.stats.msgs_sent == 0);
  CHECK(err_count[US_ERR_RADIO] >= 1);
}

/* "Sent, unconfirmed": the last chunk went out, the answer did not come. Not a failed chunk: once
 * more in the same session, then the message counts as sent. */
static void test_unconfirmed(void) {
  uint8_t msg[2100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 6);
  /* unconfirmed once, the answer to the second send comes */
  reset();
  memset(last_flags, 0, sizeof(last_flags));
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false); /* 3 chunks */
  us_start(&s);
  CHECK(complete(f) && complete(f));
  f->fail_async = 1;
  f->fail_err = US_SENT_UNCONFIRMED;
  CHECK(complete(f));
  CHECK(s.state == US_BACKOFF && s.stats.unconf_retries == 1 && s.stats.retries == 0);
  CHECK(s.q.count == 1 && s.head_fails == 0 && err_count[US_ERR_RADIO] == 0 && f->n == 3);
  CHECK(sched_delay >= US_RETRY_BASE_MS && sched_delay <= US_RETRY_BASE_MS * 3 / 2);
  clock_ms += sched_delay;
  us_run(&s);
  CHECK(f->n == 4 && f->chunks[3][1] == 2 && last_flags[3]); /* the same chunk, still last */
  CHECK(complete(f));
  CHECK(s.stats.msgs_sent == 1 && s.stats.unconfirmed == 0 && s.q.count == 0 && s.state == US_IDLE &&
        err_count[US_ERR_RADIO] == 0);

  /* unconfirmed twice: the message counts as sent, no failed session, the next one goes */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, 100, false);
  us_submit(&s, msg, 60, false);
  f->fail_async = 2;
  f->fail_err = US_SENT_UNCONFIRMED;
  us_start(&s);
  CHECK(complete(f) && s.state == US_BACKOFF);
  clock_ms += sched_delay;
  us_run(&s);
  CHECK(complete(f)); /* unconfirmed again: sent */
  CHECK(s.stats.msgs_sent == 1 && s.stats.unconfirmed == 1 && s.q.count == 1 && s.state == US_SENDING && f->n == 3 &&
        f->lens[2] == 63);
  CHECK(complete(f) && s.stats.msgs_sent == 2 && s.stats.unconfirmed == 1 && s.q.count == 0);
  CHECK(err_count[US_ERR_RADIO] == 0 && s.stats.msgs_dropped == 0 && s.head_fails == 0);

  /* three messages of a session each get an unconfirmed pair: nothing fails, nothing is dropped
   */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  for (int i = 0; i < 3; i++) {
    us_submit(&s, msg, 100, false);
  }
  f->fail_async = 10;
  f->fail_err = US_SENT_UNCONFIRMED;
  us_start(&s);
  for (int i = 0; i < 20 && s.state != US_IDLE; i++) {
    if (!complete(f)) {
      clock_ms += sched_delay;
      us_run(&s);
    }
  }
  CHECK(s.stats.msgs_sent == 3 && s.q.count == 0 && s.head_fails == 0 && err_count[US_ERR_RADIO] == 0);

  /* an error below 0 on the retry is a failed chunk as before */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  us_submit(&s, msg, 100, false);
  f->fail_async = 1;
  f->fail_err = US_SENT_UNCONFIRMED;
  us_start(&s);
  CHECK(complete(f) && s.state == US_BACKOFF);
  clock_ms += sched_delay;
  us_run(&s);
  f->fail_async = 1;
  f->fail_err = -EIO;
  CHECK(complete(f) && s.state == US_IDLE && s.q.count == 1 && s.head_fails == 1);
}

/* A report message: the app is told when it was sent or dropped. */
static void test_report_flag(void) {
  uint8_t msg[100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 7);
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  CHECK(us_submit_flags(&s, msg, 60, true, MQ_FLAG_REPORT) == 0);
  CHECK(us_submit(&s, msg, 50, true) == 0);
  CHECK(us_radio_ready(&s));
  us_start(&s);
  CHECK(complete(f) && n_end_report == 1); /* the report; the plain message does not count */
  CHECK(complete(f) && n_end_report == 1 && s.stats.msgs_sent == 2);

  /* dropped after the failed sessions */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 0);
  CHECK(us_submit_flags(&s, msg, 60, true, MQ_FLAG_REPORT) == 0);
  f->fail_async = 100;
  f->fail_err = -EIO;
  for (int i = 0; i < US_MAX_SESSION_FAILS; i++) {
    us_start(&s);
    CHECK(complete(f));
  }
  CHECK(n_end_dropped == 1 && n_end_report == 0 && s.qa.count == 0);

  /* evicted by newer messages */
  reset();
  us_register(&s, &r_lte_ra);
  fk[2].ready = false;
  CHECK(us_submit_flags(&s, msg, 60, true, MQ_FLAG_REPORT) == 0);
  for (int i = 0; i < 40; i++) {
    us_submit(&s, msg, 100, true);
  }
  CHECK(n_end_dropped == 1);
  CHECK(!us_radio_ready(&s));
}

static void test_defer_rule(void) {
  const uint32_t h = 3600000u;

  CHECK(us_defer_ok(false, h, false, 0, 1000, 100, 1000));  /* first deferral */
  CHECK(!us_defer_ok(true, h, false, 0, 1000, 100, 1000));  /* urgent */
  CHECK(!us_defer_ok(false, 0, false, 0, 1000, 100, 1000)); /* off */
  CHECK(!us_defer_ok(false, h, false, 0, 1000, 500, 1000)); /* queue half full */
  CHECK(us_defer_ok(false, h, false, 0, 1000, 499, 1000));
  CHECK(us_defer_ok(false, h, true, 1000, 1000 + h - 1, 100, 1000));  /* inside the window */
  CHECK(!us_defer_ok(false, h, true, 1000, 1000 + h, 100, 1000));     /* the window is over */
  CHECK(us_defer_ok(false, h, true, 0xFFFFFF00u, 0x100u, 100, 1000)); /* the clock wraps */
  CHECK(!us_defer_ok(false, h, true, 5, 5 + h + 7, 100, 1000));
}

static void test_link_deferral(void) {
  uint8_t msg[100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 9);

  /* a bad link holds the data, a recheck is scheduled, the bound ends the wait */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u); /* 1 h */
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  n_link_checks = 0;
  us_start(&s);
  CHECK(s.state == US_IDLE && s.q.count == 1 && f->n == 0 && s.stats.link_deferred == 1 && s.deferring);
  CHECK(sched_delay == US_DEFER_RECHECK_MS);
  for (int i = 2; i <= 4; i++) { /* a recheck every 15 min, 4 times in the first 45 min: still bad */
    clock_ms += US_DEFER_RECHECK_MS;
    us_run(&s);
    CHECK(f->n == 0 && s.stats.link_deferred == (uint32_t)i && s.deferring);
  }
  CHECK(n_link_checks == 4);
  /* 1 h after the first hold: the data go although the link is still bad */
  clock_ms += US_DEFER_RECHECK_MS;
  us_run(&s);
  CHECK(f->n == 1 && s.state == US_SENDING && !s.deferring && n_link_checks == 5);
  CHECK(complete(f) && s.stats.msgs_sent == 1);

  /* a bound that is not a multiple of the recheck time: the last look is at the bound */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 1000000u);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  us_start(&s);
  clock_ms += US_DEFER_RECHECK_MS;
  us_run(&s);
  CHECK(f->n == 0 && sched_delay == 100001u); /* 1000000 - 900000 + 1 */
  clock_ms += 100001u;
  us_run(&s);
  CHECK(f->n == 1);
  CHECK(complete(f));

  /* the link gets good: the data go at the next try */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  us_start(&s);
  CHECK(f->n == 0 && s.deferring);
  link_state = US_LINK_OK;
  clock_ms += US_DEFER_RECHECK_MS;
  us_run(&s);
  CHECK(f->n == 1 && !s.deferring);
  CHECK(complete(f) && s.stats.msgs_sent == 1);

  /* an alert is never held, and the data ride along in its session without a check */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u);
  us_submit(&s, msg, sizeof(msg), false); /* data */
  us_submit(&s, msg, 40, true);           /* alert */
  link_state = US_LINK_BAD;
  n_link_checks = 0;
  us_start(&s);
  CHECK(f->n == 1 && f->lens[0] == 43 && n_link_checks == 0); /* the alert, sent at once */
  CHECK(complete(f) && f->n == 2 && f->lens[1] == 103);       /* then the data, no check */
  CHECK(complete(f) && s.stats.msgs_sent == 2 && n_link_checks == 0 && s.state == US_IDLE);

  /* data alone are held again afterwards (the session ended) */
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(f->n == 2 && n_link_checks == 1 && s.stats.link_deferred == 1);

  /* a half full queue is not held: nothing is dropped to wait */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u);
  {
    uint8_t big[1000];

    make_msg(big, sizeof(big), 1);
    for (int i = 0; i < 7; i++) {
      us_submit(&s, big, sizeof(big), false); /* 7 x 1004 B of 16384 B: below half */
    }
    link_state = US_LINK_BAD;
    us_start(&s);
    CHECK(s.stats.link_deferred == 1 && f->n == 0);
    for (int i = 0; i < 2; i++) {
      us_submit(&s, big, sizeof(big), false); /* 9 x 1004 B: more than half */
    }
    clock_ms += US_DEFER_RECHECK_MS;
    us_run(&s);
    CHECK(f->n == 1); /* sent although the link is bad */
  }

  /* deferral off: the radio is not asked */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  n_link_checks = 0;
  us_start(&s);
  CHECK(f->n == 1 && n_link_checks == 0);

  /* the radio evaluates: wait, its up hook (us_start) or the timer asks again */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_WAIT;
  us_start(&s);
  CHECK(f->n == 0 && s.state == US_IDLE && s.stats.link_waits == 1 && sched_delay == US_LINK_WAIT_MS &&
        s.deferring); /* the wait counts as the deferral */
  link_state = US_LINK_OK;
  us_start(&s); /* on_up after the evaluation */
  CHECK(f->n == 1);
  CHECK(complete(f) && s.stats.msgs_sent == 1);
  /* the event never came: the timer looks again */
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_WAIT;
  us_start(&s);
  link_state = US_LINK_OK;
  clock_ms += US_LINK_WAIT_MS;
  us_run(&s);
  CHECK(f->n == 2);
}

/* A radio that never finishes its evaluation (the event is lost) must not hold the data for ever:
 * the wait is bound by the deferral time, then the message goes as if the link were fine. */
static void test_link_wait_bound(void) {
  uint8_t msg[100];
  struct fake *f = &fk[2];
  const uint32_t window = 3600000u;
  int waits = 0;

  make_msg(msg, sizeof(msg), 9);
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, window);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_WAIT;
  us_start(&s);
  while (f->n == 0 && waits < 1000) {
    waits++;
    clock_ms += US_LINK_WAIT_MS;
    us_run(&s);
  }
  CHECK(f->n == 1 && waits < 1000);
  CHECK(waits >= (int)(window / US_LINK_WAIT_MS) && waits <= (int)(window / US_LINK_WAIT_MS) + 2);
  CHECK(s.stats.link_wait_timeouts == 1 && !s.deferring && s.state == US_SENDING);
  CHECK(complete(f) && s.stats.msgs_sent == 1);

  /* a wait that follows a bad result shares its window */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, window);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  us_start(&s);
  CHECK(s.deferring && f->n == 0);
  link_state = US_LINK_WAIT;
  clock_ms += window - 1000;
  us_run(&s);
  CHECK(f->n == 0 && s.deferring);
  clock_ms += 1000;
  us_run(&s);
  CHECK(f->n == 1 && !s.deferring);

  /* a data queue that is half full does not wait */
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, window);
  for (int i = 0; i < 90; i++) {
    us_submit(&s, msg, sizeof(msg), false);
  }
  link_state = US_LINK_WAIT;
  us_start(&s);
  CHECK(f->n == 1);
  link_state = US_LINK_OK;
}

/* The deferral ends with the data: an alert session that sends the data queue empty clears it. */
static void test_deferral_cleared_when_empty(void) {
  uint8_t msg[100];
  struct fake *f = &fk[2];

  make_msg(msg, sizeof(msg), 10);
  reset();
  us_register(&s, &r_lte_ra);
  us_set_config(&s, sensei_Radio_RADIO_LTEM, 2);
  us_set_defer(&s, 3600000u);
  us_submit(&s, msg, sizeof(msg), false);
  link_state = US_LINK_BAD;
  us_start(&s);
  CHECK(s.deferring && f->n == 0 && s.q.count == 1);
  us_submit(&s, msg, 40, true); /* an alert: the session sends it and the data behind it */
  us_start(&s);
  CHECK(f->n == 1);
  CHECK(complete(f) && f->n == 2 && s.q.count == 1);
  CHECK(complete(f) && s.q.count == 0 && s.qa.count == 0 && s.state == US_IDLE);
  CHECK(!s.deferring);
  /* the next data message gets a new window */
  clock_ms += 1000;
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(s.deferring && s.defer_since_ms == clock_ms && f->n == 2);
  link_state = US_LINK_OK;
}

/* G1: a radio can set its own chunk timeout (LoRa waits for the duty cycle). */
static void test_radio_chunk_timeout(void) {
  static const struct uplink_radio r_slow = {
      .id = sensei_Radio_RADIO_LORA, .mtu = 222, .ready = ready1, .send = send1, .chunk_timeout_ms = 660000u};
  uint8_t msg[100];

  make_msg(msg, sizeof(msg), 2);
  CHECK(US_STALL_MS > r_slow.chunk_timeout_ms && r_slow.chunk_timeout_ms > US_CHUNK_TIMEOUT_MS);
  reset();
  us_register(&s, &r_slow);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(sched_delay == 660000u);
  clock_ms += US_CHUNK_TIMEOUT_MS + 1000; /* the default limit is past: still waiting */
  us_run(&s);
  CHECK(s.state == US_SENDING && s.stats.timeouts == 0 && sched_delay == 660000u - US_CHUNK_TIMEOUT_MS - 1000);
  CHECK(!us_stalled(&s, clock_ms, US_STALL_MS));
  clock_ms += 660000u;
  us_run(&s);
  CHECK(s.state == US_IDLE && s.stats.timeouts == 1);
  CHECK(!us_stalled(&s, clock_ms + US_STALL_MS, US_STALL_MS)); /* idle again */
}

static void test_jitter_and_stall(void) {
  uint8_t msg[100];

  CHECK(us_retry_delay_ms(0, 0) == US_RETRY_BASE_MS);
  CHECK(us_retry_delay_ms(0, 0xFFFFFFFFu) >= US_RETRY_BASE_MS &&
        us_retry_delay_ms(0, 0xFFFFFFFFu) <= US_RETRY_BASE_MS * 3 / 2);
  CHECK(us_retry_delay_ms(1, 7) == 2 * US_RETRY_BASE_MS + 7);
  CHECK(us_retry_delay_ms(30, 0xFFFFFFFFu) <= US_RETRY_MAX_MS * 3 / 2);
  CHECK(us_retry_delay_ms(30, 0) == US_RETRY_MAX_MS);

  /* The scheduler adds the jitter from ops. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 1);
  make_msg(msg, sizeof(msg), 1);
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  rand_value = 777;
  fk[1].fail_async = 1;
  complete(&fk[1]);
  CHECK(sched_delay == US_RETRY_BASE_MS + 777 % (US_RETRY_BASE_MS / 2 + 1));

  /* Stall: a session in progress without progress for stall_ms. */
  reset();
  us_register(&s, &r_lora);
  us_set_config(&s, sensei_Radio_RADIO_LORA, 0);
  CHECK(!us_stalled(&s, clock_ms + 10 * US_STALL_MS, US_STALL_MS)); /* idle */
  us_submit(&s, msg, sizeof(msg), false);
  us_start(&s);
  CHECK(!us_stalled(&s, clock_ms + US_STALL_MS, US_STALL_MS));
  CHECK(us_stalled(&s, clock_ms + US_STALL_MS + 1, US_STALL_MS));
  clock_ms += US_STALL_MS;
  complete(&fk[1]); /* progress */
  CHECK(!us_stalled(&s, clock_ms + 1, US_STALL_MS));
}

int main(void) {
  test_fragmentation();
  test_one_chunk_at_a_time();
  test_mtu_fixed_per_message();
  test_prepare_at_send_time();
  test_multiple_messages();
  test_retries();
  test_fallback();
  test_queue_limits();
  test_priority_queue();
  test_too_many_chunks();
  test_abort();
  test_tick();
  test_node_offset();
  test_timeout();
  test_stale_completion();
  test_late_completion_after_a_radio_switch();
  test_mtu_now_and_restart_on_emsgsize();
  test_deferral();
  test_deferral_resume_rules();
  test_not_connected_deferral();
  test_last_chunk();
  test_no_answer();
  test_unconfirmed();
  test_report_flag();
  test_defer_rule();
  test_link_deferral();
  test_link_wait_bound();
  test_deferral_cleared_when_empty();
  test_radio_chunk_timeout();
  test_jitter_and_stall();
  DONE();
}
