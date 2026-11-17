/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Link state machine against a simulated GAP9 master, with a plain binary job and result format. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sensei_gap9/gap9_core.h"
#include "sensei_link/crc.h"
#include "sensei_link/pb.h"

/* ---- the test's job and result: raw structs, no schema ---- */

enum { TK_PLAIN = 1, TK_BUS = 2 }; /* TK_BUS: needs the camera bus, like a detect job */
typedef struct {
  uint32_t seq;
  uint8_t kind;
  uint8_t jpeg;
  uint8_t data[720];
} tjob;

enum { TR_PLAIN = 1, TR_BUS = 2, TR_CHUNK = 3 };
enum { TS_OK = 0, TS_NOT_BUILT = 5 };
typedef struct {
  uint32_t seq;
  uint8_t kind;
  uint8_t status;
  uint32_t n;
  uint8_t data[1024];
} tres;

static int32_t t_encode(uint8_t *buf, size_t cap, const void *job) {
  if (cap < sizeof(tjob)) {
    return -1;
  }
  memcpy(buf, job, sizeof(tjob));
  return (int32_t)sizeof(tjob);
}
static bool t_decode_result(const uint8_t *buf, size_t len, void *res) {
  if (len != sizeof(tres)) {
    return false;
  }
  memcpy(res, buf, len);
  return true;
}
static bool t_decode_job(const uint8_t *buf, size_t len, void *job) {
  if (len != sizeof(tjob)) {
    return false;
  }
  memcpy(job, buf, len);
  return true;
}
static uint32_t t_job_seq(const void *j) { return ((const tjob *)j)->seq; }
static void t_job_set_seq(void *j, uint32_t seq) { ((tjob *)j)->seq = seq; }
static uint32_t t_result_seq(const void *r) { return ((const tres *)r)->seq; }
static int t_result_err(const void *r) {
  switch (((const tres *)r)->status) {
  case TS_OK:
    return 0;
  case TS_NOT_BUILT:
    return -ENOTSUP;
  default:
    return -EIO;
  }
}
static bool t_uses_bus(const void *j) { return ((const tjob *)j)->kind == TK_BUS; }

#define TEST_GAP9_MAJOR 1 /* sim_hello() reports 1.2.3 */

static const struct g9_codec codec = {
    .job_size = sizeof(tjob),
    .result_size = sizeof(tres),
    .encode_job = t_encode,
    .decode_result = t_decode_result,
    .job_seq = t_job_seq,
    .job_set_seq = t_job_set_seq,
    .result_seq = t_result_seq,
    .result_err = t_result_err,
    .job_uses_bus = t_uses_bus,
    .gap9_major = TEST_GAP9_MAJOR,
};
static tjob jobs_pool[G9_QUEUE_DEPTH];
static tres res_buf;

static int fails, checks;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    checks++;                                                                                                          \
    if (!(c)) {                                                                                                        \
      fails++;                                                                                                         \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                                              \
    }                                                                                                                  \
  } while (0)

/* ---- fake platform with an event log ---- */

static char evlog[4096];
static void ev(const char *s) {
  strncat(evlog, s, sizeof(evlog) - strlen(evlog) - 2);
  strncat(evlog, " ", sizeof(evlog) - strlen(evlog) - 1);
}
static bool has(const char *s) { return strstr(evlog, s) != NULL; }
static int idx(const char *s) {
  const char *p = strstr(evlog, s);
  return p ? (int)(p - evlog) : -1;
}

static bool powered, trig, locked;
static int lock_n, unlock_n;
static uint32_t timer_ms;
static int timer_n; /* timers started */
static struct g9 g;

static void op_power(bool on) {
  powered = on;
  ev(on ? "P1" : "P0");
}
/* The frame is g.tx. Log what is offered when the line goes high. */
static void op_trigger(bool h) {
  trig = h;
  if (h) {
    ev(g.tx[2] == G9L_TYPE_JOB ? "armJ" : "armN");
  }
}
static void op_lock(void) {
  CHECK(!locked);
  locked = true;
  lock_n++;
  ev("L");
}
static void op_unlock(void) {
  CHECK(locked);
  locked = false;
  unlock_n++;
  ev("U");
}
static void op_timer(uint32_t ms) {
  timer_ms = ms;
  if (ms != 0) {
    timer_n++;
  }
}

struct got {
  int err;
  int which;
  uint32_t seq;
  int n;
  bool final;
};
static struct got gots[64];
static int ngot;
static void deliver(g9_done_fn fn, void *user, int err, const void *r, bool final) { fn(err, r, final, user); }
static void done_cb(int err, const void *res, bool final, void *user) {
  const tres *r = res;
  struct got *x = &gots[ngot++];

  (void)user;
  x->final = final;
  x->err = err;
  x->which = r ? r->kind : -1;
  x->seq = r ? r->seq : 0;
  x->n = r && r->kind == TR_BUS ? (int)r->n : -1;
  ev(r && r->kind == TR_CHUNK ? "Dj" : "D");
}
static unsigned nerr;
static void op_error(void) { nerr++; }
static void op_marker(bool on) { (void)on; }

static const struct g9_ops ops = {op_power, op_trigger, op_lock, op_unlock, op_timer, deliver, op_error, op_marker};
static const struct g9_cfg cfg = {4000, 3000, 8000, 200, 150};

static void reset(void) {
  memset(evlog, 0, sizeof(evlog));
  powered = trig = locked = false;
  lock_n = unlock_n = 0;
  timer_ms = 0;
  timer_n = 0;
  ngot = 0;
  nerr = 0;
  g9_init(&g, &ops, &codec, &cfg, jobs_pool, &res_buf);
}

/* ---- simulated GAP9 master ---- */

static uint8_t frame[G9L_TX_CAP];
static uint8_t pollbuf[G9L_TX_CAP];

static void sim_push(uint8_t type, uint8_t flags, const uint8_t *p, size_t len, int corrupt) {
  size_t n = sensei_spi_frame_encode(type, flags, p, len, frame, sizeof(frame));

  CHECK(trig); /* GAP9 only starts a transaction when the nRF is armed */
  if (corrupt && len > 0) {
    frame[G9L_HDR] ^= 0x40;
  }
  g9_spi_done(&g, frame, n);
}

static void sim_hello_major(uint32_t major) {
  sensei_Version v = {major, 2, 3, 0xabcd};
  uint8_t b[32];
  int32_t n = sensei_pb_encode(b, sizeof(b), &sensei_Version_msg, &v);

  sim_push(G9L_TYPE_HELLO, 0, b, (size_t)n, 0);
}

static void sim_hello(void) { sim_hello_major(TEST_GAP9_MAJOR); }

static void sim_result(const tres *r, uint8_t flags, int corrupt) {
  sim_push(G9L_TYPE_RESULT, flags, (const uint8_t *)r, sizeof(*r), corrupt);
}

/* POLL. full: read the whole frame (fetch), else the header only (peek). Returns the header of
 * the armed frame; the payload CRC is checked and *bad is set if it fails (GAP9 would retry). */
static sensei_spi_header_t sim_poll(bool full, bool force_err, tjob *job, bool *bad, int flip) {
  uint8_t hdr[G9L_HDR];
  sensei_spi_header_t h;
  const uint8_t *p = NULL;
  uint8_t copy[G9L_TX_CAP];

  CHECK(trig);
  sensei_spi_header_pack(hdr, full ? G9L_TYPE_POLL : G9L_TYPE_PEEK, force_err ? G9L_FLAG_ERROR : 0, NULL, 0);
  memcpy(copy, g.tx, g.tx_len);
  size_t clocked = full ? g.tx_len : G9L_HDR;

  if (full && flip && g.tx_len > G9L_HDR) {
    copy[G9L_HDR] ^= 0x01;
  }
  CHECK(sensei_spi_header_parse(copy, &h) == SENSEI_SPI_OK);
  *bad = false;
  if (full && h.len > 0) {
    *bad = sensei_spi_frame_decode(copy, g.tx_len, &h, &p) != SENSEI_SPI_OK;
    if (!*bad && job != NULL && h.type == G9L_TYPE_JOB) {
      *bad = !t_decode_job(p, h.len, job);
    }
  }
  /* the header is read before the master decides; flags of the armed frame stay in h */
  sensei_spi_header_t out;
  sensei_spi_header_parse(g.tx, &out);
  memset(pollbuf, 0, sizeof(pollbuf));
  memcpy(pollbuf, hdr, G9L_HDR);
  g9_spi_done(&g, pollbuf, clocked);
  return out;
}

static tjob job_forecast(void) {
  tjob j;

  memset(&j, 0, sizeof(j));
  j.kind = TK_PLAIN;
  return j;
}
static tjob job_detect(bool jpeg) {
  tjob j;

  memset(&j, 0, sizeof(j));
  j.kind = TK_BUS;
  j.jpeg = jpeg;
  return j;
}
static void res_forecast(tres *r, uint32_t seq) {
  memset(r, 0, sizeof(*r));
  r->seq = seq;
  r->kind = TR_PLAIN;
}
static void res_detect(tres *r, uint32_t seq, uint32_t n) {
  memset(r, 0, sizeof(*r));
  r->seq = seq;
  r->kind = TR_BUS;
  r->n = n;
}

/* GAP9 side of one normal job: fetch, run, push the result. */
static uint32_t gap9_do_job(void) {
  tjob j;
  bool bad;
  tres r;
  sensei_spi_header_t h = sim_poll(true, false, &j, &bad, 0);

  CHECK(h.type == G9L_TYPE_JOB && !bad);
  if (j.kind == TK_BUS) {
    sim_poll(false, false, NULL, &bad, 0); /* nothing: camera not yet done */
    uint32_t s = j.seq;

    sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
    res_detect(&r, j.seq, 2);
  } else {
    res_forecast(&r, j.seq);
  }
  sim_result(&r, 0, 0);
  return j.seq;
}

/* ---- tests ---- */

static void test_normal(void) {
  tjob j = job_forecast();
  tjob j2 = job_forecast();
  bool bad;

  reset();
  CHECK(!g9_busy(&g));
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  CHECK(g9_submit(&g, &j2, done_cb, NULL) == 0);
  CHECK(powered && trig && timer_ms == cfg.boot_ms);
  CHECK(g9_busy(&g));
  /* during boot only NONE is offered, never a job */
  CHECK(g.tx_len == G9L_HDR && has("armN") && !has("armJ"));
  sim_hello();
  CHECK(g.version.major == 1 && g.version.patch == 3 && g.version.git == 0xabcd);
  CHECK(has("armJ") && trig && timer_ms == cfg.job_ms && g.timer == G9T_FETCH); /* GAP9 has to fetch it */

  CHECK(gap9_do_job() == 1);
  CHECK(trig && ngot == 1 && gots[0].err == 0 && gots[0].seq == 1 && gots[0].final);
  CHECK(gap9_do_job() == 2);
  CHECK(ngot == 2 && gots[1].seq == 2);

  /* queue is empty: NONE is served, trigger drops, the idle timer runs */
  sensei_spi_header_t h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && !bad);
  CHECK(!trig && timer_ms == cfg.idle_off_ms && powered);
  g9_timer_expired(&g);
  CHECK(!powered && !g9_busy(&g) && nerr == 0);
}

static void test_result_crc_error(void) {
  tjob j = job_forecast();
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  res_forecast(&r, fj.seq);
  sim_result(&r, 0, 1); /* corrupted payload */
  CHECK(ngot == 0 && nerr == 1);
  sensei_spi_header_t h = sim_poll(false, false, NULL, &bad, 0); /* GAP9 peeks the ack */
  CHECK((h.flags & G9L_FLAG_ERROR) != 0);
  sim_result(&r, 0, 0); /* resend */
  CHECK(ngot == 1 && gots[0].err == 0);
  h = sim_poll(true, false, NULL, &bad, 0);
  CHECK((h.flags & G9L_FLAG_ERROR) == 0 && h.type == G9L_TYPE_NONE);
}

static void test_job_crc_error(void) {
  tjob j = job_forecast();
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 1); /* GAP9 reads a flipped bit ... */
  CHECK(bad);
  /* ... and polls again with ERROR: nothing was consumed, the job is still in flight */
  sim_poll(true, true, &fj, &bad, 0);
  CHECK(timer_ms == cfg.job_ms || timer_ms != 0); /* in-flight timer is running */
  sensei_spi_header_t h = sim_poll(true, false, &fj, &bad, 0);
  CHECK(h.type == G9L_TYPE_JOB && !bad && fj.seq == 1);
  res_forecast(&r, fj.seq);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].err == 0 && nerr >= 1);
}

static void test_bad_header_from_gap9(void) {
  uint8_t junk[16] = {1, 2, 3};

  reset();
  tjob j = job_forecast();
  g9_submit(&g, &j, done_cb, NULL);
  g9_spi_done(&g, junk, sizeof(junk));
  CHECK(nerr == 1 && trig);
  CHECK((g.tx[3] & G9L_FLAG_ERROR) != 0);
}

static void test_job_timeout_and_restart(void) {
  tjob j = job_forecast();
  tjob fj;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  CHECK(timer_ms == cfg.job_ms);
  g9_timer_expired(&g); /* no result */
  CHECK(ngot == 1 && gots[0].err == -ETIMEDOUT && nerr == 1);
  CHECK(!powered && timer_ms == cfg.restart_off_ms); /* power cycle, second job waits */
  g9_timer_expired(&g);
  CHECK(powered && timer_ms == cfg.boot_ms);
  sim_hello();
  CHECK(gap9_do_job() == 2);
  CHECK(ngot == 2 && gots[1].err == 0);
}

static void test_boot_timeout(void) {
  tjob j = job_forecast();

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  g9_timer_expired(&g); /* first boot fails: one retry */
  CHECK(!powered && ngot == 0 && timer_ms == cfg.restart_off_ms);
  g9_timer_expired(&g);
  CHECK(powered && timer_ms == cfg.boot_ms);
  g9_timer_expired(&g); /* second boot fails: give up */
  CHECK(!powered && ngot == 1 && gots[0].err == -ETIMEDOUT && !g9_busy(&g));
}

static void test_bus_free_order(void) {
  tjob j = job_detect(false);
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(!has("L")); /* not while booting */
  sim_hello();
  CHECK(idx("L") >= 0 && idx("L") < idx("armJ")); /* locked before the job is offered */
  CHECK(locked);
  sim_poll(true, false, &fj, &bad, 0);
  CHECK(timer_ms == cfg.busfree_ms && locked);
  uint32_t s = fj.seq;

  sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
  CHECK(!locked && unlock_n == 1 && timer_ms == cfg.job_ms);
  CHECK(ngot == 0); /* the result comes later */
  res_detect(&r, fj.seq, 3);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].n == 3 && lock_n == 1 && unlock_n == 1 && nerr == 0);
  CHECK(idx("U") < idx("D"));

  /* result without bus free: error, lock released anyway */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  res_detect(&r, fj.seq, 1);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && !locked && nerr == 1);

  /* bus free timeout releases the lock, but only after GAP9 is off (the camera may still use the bus) */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  g9_timer_expired(&g);
  CHECK(!locked && ngot == 1 && gots[0].err == -ETIMEDOUT);
  CHECK(idx("P0") >= 0 && idx("P0") < idx("U") && idx("U") < idx("D"));

  /* power off with a locked, unfetched detect job */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  CHECK(locked);
  g9_shutdown(&g);
  CHECK(!locked && ngot == 1 && gots[0].err == -ECANCELED && !powered);
}

static void test_jpeg_chunks(void) {
  tjob j = job_detect(true);
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  uint32_t s = fj.seq;

  sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
  res_detect(&r, fj.seq, 2);
  sim_result(&r, G9L_FLAG_MORE, 0); /* detect result first, then the chunks */
  CHECK(ngot == 1 && gots[0].which == TR_BUS && gots[0].n == 2 && !gots[0].final);
  CHECK(!locked);
  for (uint32_t i = 0; i < 40; i++) {
    memset(&r, 0, sizeof(r));
    r.seq = fj.seq;
    r.kind = TR_CHUNK;
    r.n = i;
    sim_result(&r, i < 39 ? G9L_FLAG_MORE : 0, 0);
    if (i < 39) {
      sensei_spi_header_t h = sim_poll(false, false, NULL, &bad, 0);

      CHECK((h.flags & G9L_FLAG_ERROR) == 0);
      CHECK(timer_ms == cfg.job_ms); /* in flight until the last chunk */
    }
  }
  CHECK(ngot == 41 && gots[40].which == TR_CHUNK && gots[40].err == 0 && gots[40].final && !gots[39].final);
  CHECK(nerr == 0 && !locked);
  sensei_spi_header_t h2 = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h2.type == G9L_TYPE_NONE); /* job finished: next frame is NONE */
}

static void test_idle_and_queue(void) {
  tjob j = job_forecast();
  bool bad;

  reset();
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  }
  CHECK(g9_full(&g) && g9_submit(&g, &j, done_cb, NULL) == -ENOMEM);
  sim_hello();
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    gap9_do_job();
  }
  CHECK(ngot == G9_QUEUE_DEPTH);
  for (int i = 0; i < G9_QUEUE_DEPTH; i++) {
    CHECK(gots[i].seq == (uint32_t)i + 1); /* FIFO */
  }
  sim_poll(true, false, NULL, &bad, 0); /* NONE served */
  CHECK(!trig && timer_ms == cfg.idle_off_ms);

  /* a job during the idle delay: trigger goes high, idle timer stops, no power cycle */
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  CHECK(trig && g.timer == G9T_FETCH && powered); /* idle timer stopped */
  CHECK(gap9_do_job() == G9_QUEUE_DEPTH + 1);
  sim_poll(true, false, NULL, &bad, 0);
  CHECK(!trig && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered);

  /* a new job after power-off boots again */
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  CHECK(powered && trig);
  /* an idle timer that fires with a job in flight must not power off */
  sim_hello();
  bool b2;
  tjob fj;
  sim_poll(true, false, &fj, &b2, 0);
  g.timer = 4; /* G9T_IDLE */
  g9_timer_expired(&g);
  CHECK(powered);
}

static void test_status_mapping(void) {
  tjob j = job_forecast();
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  res_forecast(&r, fj.seq);
  r.status = TS_NOT_BUILT;
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].err == -ENOTSUP);
  /* a result with a wrong seq is dropped */
  g9_submit(&g, &j, done_cb, NULL);
  sim_poll(true, false, &fj, &bad, 0);
  res_forecast(&r, fj.seq + 5);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1);
}

/* A POLL header that fails its check must not wedge the link (it did: the ERROR flag stayed set,
 * GAP9 skipped the payload and polled for ever). */
static void test_corrupt_poll_recovers(void) {
  tjob j = job_forecast();
  tjob fj;
  tres r;
  bool bad;
  uint8_t junk[G9L_HDR];

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sensei_spi_header_pack(junk, G9L_TYPE_POLL, 0, NULL, 0);
  junk[6] ^= 0xff;
  g9_spi_done(&g, junk, G9L_HDR);
  CHECK(g.err_flag && (g.tx[3] & G9L_FLAG_ERROR) != 0);
  /* GAP9 polls (its poll is good): it reads the job although the header has ERROR */
  sensei_spi_header_t h = sim_poll(true, false, &fj, &bad, 0);
  CHECK(h.type == G9L_TYPE_JOB && (h.flags & G9L_FLAG_ERROR) != 0 && !bad && fj.seq == 1);
  CHECK(!g.err_flag && g.timer == G9T_JOB);
  res_forecast(&r, fj.seq);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].err == 0);
}

/* GAP9 never takes the armed job (it only peeks, or it is dead): fail it and power cycle. */
static void test_fetch_timeout(void) {
  tjob jd = job_detect(false);
  tjob jf = job_forecast();
  bool bad;

  reset();
  g9_submit(&g, &jd, done_cb, NULL);
  g9_submit(&g, &jf, done_cb, NULL);
  sim_hello();
  CHECK(locked && g.timer == G9T_FETCH && timer_ms == cfg.job_ms);
  int n = timer_n;

  for (int i = 0; i < 5; i++) {
    sim_poll(false, false, NULL, &bad, 0); /* peeks do not restart it */
  }
  CHECK(timer_n == n && g.timer == G9T_FETCH);
  g9_timer_expired(&g);
  CHECK(!powered && !locked && ngot == 1 && gots[0].err == -ETIMEDOUT && nerr == 1);
  CHECK(idx("P0") < idx("U"));           /* GAP9 off before the bus is released */
  CHECK(timer_ms == cfg.restart_off_ms); /* the second job boots again */
  g9_timer_expired(&g);
  sim_hello();
  CHECK(gap9_do_job() == 2 && ngot == 2 && gots[1].err == 0);
}

/* GAP9 resets itself while a camera job runs: HELLO in RUN. The job starts again, with the bus lock. */
static void test_gap9_reset_in_run(void) {
  tjob j = job_detect(false);
  tjob fj, again;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  uint32_t s = fj.seq;

  sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
  CHECK(!locked && lock_n == 1);
  sim_hello(); /* watchdog reset of GAP9 */
  CHECK(nerr == 1 && locked && lock_n == 2 && ngot == 0);
  sensei_spi_header_t h = sim_poll(true, false, &again, &bad, 0);

  CHECK(h.type == G9L_TYPE_JOB && again.seq == fj.seq && !bad);
  sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
  CHECK(!locked && nerr == 1); /* the second bus free is not an error */
  res_detect(&r, fj.seq, 2);
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].err == 0 && gots[0].final && lock_n == 2 && unlock_n == 2);

  /* some result frames were delivered already: the job fails, it is not run twice */
  tjob jj = job_detect(true);

  reset();
  g9_submit(&g, &jj, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  s = fj.seq;
  sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
  res_detect(&r, fj.seq, 2);
  sim_result(&r, G9L_FLAG_MORE, 0);
  CHECK(ngot == 1 && !gots[0].final);
  sim_hello();
  CHECK(ngot == 2 && gots[1].err == -ECONNRESET && gots[1].final && !locked && powered);
  CHECK(g.q[0].st == G9E_FREE);
}

/* A header-only look (PEEK) never takes the frame, not even a NONE of 8 bytes. */
static void test_peek_keeps_none(void) {
  tjob j = job_forecast();
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  gap9_do_job();
  CHECK(g.armed < 0 && trig);
  sensei_spi_header_t h = sim_poll(false, false, NULL, &bad, 0); /* ack of the result */
  CHECK(h.type == G9L_TYPE_NONE && trig && g.timer != G9T_IDLE);
  h = sim_poll(true, false, NULL, &bad, 0); /* the fetch takes it */
  CHECK(h.type == G9L_TYPE_NONE && !trig && g.timer == G9T_IDLE);
}

/* A job that fails before the camera was used (bad job, detector not built) sends no BUSFREE. */
static void test_early_error_frees_bus(void) {
  tjob j = job_detect(false);
  tjob fj;
  tres r;
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  CHECK(locked);
  res_detect(&r, fj.seq, 0);
  r.status = TS_NOT_BUILT;
  sim_result(&r, 0, 0);
  CHECK(!locked && ngot == 1 && gots[0].err == -ENOTSUP && gots[0].final);
  CHECK(nerr == 1); /* the failed job, no second error for the missing bus free */
}

/* GAP9 firmware with other job messages. */
static void test_version_mismatch(void) {
  tjob j = job_detect(false);
  uint8_t junk[8] = {0xff, 0xff, 0xff, 0xff};

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello_major(TEST_GAP9_MAJOR + 1);
  CHECK(!powered && !locked && ngot == 2 && gots[0].err == -EPROTO && gots[1].err == -EPROTO);
  CHECK(nerr == 1 && !g9_busy(&g) && g.version.major == TEST_GAP9_MAJOR + 1);

  /* a hello that does not decode counts the same */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_push(G9L_TYPE_HELLO, 0, junk, sizeof(junk), 0);
  CHECK(!powered && ngot == 1 && gots[0].err == -EPROTO);

  /* the next boot with the right firmware works */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello_major(TEST_GAP9_MAJOR + 1);
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(powered);
  sim_hello();
  CHECK(gap9_do_job() == 2 && gots[1].err == 0);
}

static void test_reset_button(void) {
  tjob j = job_forecast();
  tjob fj;
  bool bad;

  reset();
  g9_reset(&g); /* off: nothing */
  CHECK(!powered && nerr == 0 && !has("P0"));
  g9_submit(&g, &j, done_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  g9_reset(&g);
  CHECK(!powered && ngot == 1 && gots[0].err == -ECANCELED && timer_ms == cfg.restart_off_ms);
  g9_timer_expired(&g);
  sim_hello();
  CHECK(gap9_do_job() == 2 && ngot == 2 && gots[1].err == 0);
}

int main(void) {
  test_normal();
  test_result_crc_error();
  test_job_crc_error();
  test_bad_header_from_gap9();
  test_job_timeout_and_restart();
  test_boot_timeout();
  test_bus_free_order();
  test_jpeg_chunks();
  test_idle_and_queue();
  test_status_mapping();
  test_corrupt_poll_recovers();
  test_fetch_timeout();
  test_gap9_reset_in_run();
  test_peek_keeps_none();
  test_early_error_frees_bus();
  test_version_mismatch();
  test_reset_button();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
