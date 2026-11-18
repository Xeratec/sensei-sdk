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
static uint8_t slot_req;   /* what gap9_link_set_slot() / gap9_link_reboot() store (platform) */
static int power_slot[16]; /* the slot line at each power-up, like the platform sets it before gap9_pwr(true) */
static int power_ups;
static int lock_n, unlock_n;
static uint32_t timer_ms;
static int timer_n; /* timers started */
static struct g9 g;

static void op_power(bool on) {
  powered = on;
  if (on && power_ups < 16) {
    power_slot[power_ups++] = slot_req;
  }
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

/* g9_reboot() results (the platform runs the callback outside the core; here it is a plain call). */
static struct {
  int n;
  int err;
  bool has_hello;
  uint32_t major;
  void *user;
} rbk;
static void reboot_cb(int err, const sensei_Version *hello, void *user) {
  rbk.n++;
  rbk.err = err;
  rbk.has_hello = hello != NULL;
  rbk.major = hello ? hello->major : 0;
  rbk.user = user;
}
static void op_reboot_done(g9_reboot_fn fn, void *user, int err, const sensei_Version *hello) { fn(err, hello, user); }

static const struct g9_ops ops = {op_power, op_trigger, op_lock,   op_unlock,     op_timer,
                                  deliver,  op_error,   op_marker, op_reboot_done};
static const struct g9_cfg cfg = {4000, 3000, 8000, 200, 150, 2000};

static void reset(void) {
  memset(evlog, 0, sizeof(evlog));
  powered = trig = locked = false;
  lock_n = unlock_n = 0;
  timer_ms = 0;
  timer_n = 0;
  ngot = 0;
  nerr = 0;
  slot_req = 0;
  power_ups = 0;
  memset(&rbk, 0, sizeof(rbk));
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

/* The POLL that took the job is garbled on MOSI, but GAP9 clocked in the whole job. Its BUSFREE or RESULT for the armed
 * job promotes it to SENT: the job is recovered, not lost (E6). */
static void test_lost_poll_recovers(int bus) {
  tjob j = bus ? job_detect(false) : job_forecast();
  tjob fj;
  tres r;
  bool bad;
  uint8_t junk[G9L_TX_CAP] = {0};

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  memcpy(&fj, g.tx + G9L_HDR, sizeof(fj)); /* what GAP9 reads */
  sensei_spi_header_pack(junk, G9L_TYPE_POLL, 0, NULL, 0);
  junk[6] ^= 0xff;
  g9_spi_done(&g, junk, g.tx_len);
  CHECK(g.q[0].st == G9E_PENDING && g.timer == G9T_FETCH);
  uint32_t s = fj.seq;
  unsigned errors = nerr;

  if (bus) {
    sim_push(G9L_TYPE_BUSFREE, 0, (const uint8_t *)&s, 4, 0);
    CHECK(g.q[0].st == G9E_SENT && !locked && g.timer == G9T_JOB && nerr == errors);
    res_detect(&r, s, 2);
  } else {
    res_forecast(&r, s);
  }
  sim_result(&r, 0, 0);
  CHECK(ngot == 1 && gots[0].err == 0 && gots[0].final && g.q[0].st == G9E_FREE && !locked);
  /* GAP9 fetches again (same seq, it skipped the job): nothing to offer, no timeout */
  sim_poll(true, false, NULL, &bad, 0);
  CHECK(g.timer != G9T_FETCH && g.timer != G9T_JOB && powered);
}

/* A result for another seq than the armed job is not taken for it. */
static void test_foreign_result_ignored(void) {
  tjob j = job_forecast();
  tres r;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  res_forecast(&r, 99);
  sim_result(&r, 0, 0);
  CHECK(ngot == 0 && g.q[0].st == G9E_PENDING && nerr == 1);
}

/* A new job while the first waits for its fetch keeps the fetch timer (E7): a GAP9 that never fetches is power
 * cycled after one job time, not after one per queued job. */
static void test_submit_keeps_fetch_timer(void) {
  tjob j = job_forecast();
  bool bad;

  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  CHECK(g.timer == G9T_FETCH);
  int n = timer_n;

  g9_submit(&g, &j, done_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(g.timer == G9T_FETCH && timer_n == n);
  g9_timer_expired(&g);
  CHECK(!powered && ngot == 1 && gots[0].err == -ETIMEDOUT); /* only the armed job fails */

  /* an idle timer is still cancelled by a new job */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  CHECK(gap9_do_job() == 1);
  sim_poll(true, false, NULL, &bad, 0); /* "no more jobs": GAP9 sleeps */
  CHECK(g.timer == G9T_IDLE);
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(g.timer == G9T_FETCH);
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

/* ---- reboot into a slot ---- */

static void test_reboot(void) {
  tjob j = job_forecast();
  tjob fj;
  bool bad;
  int tag;

  reset();
  CHECK(g9_reboot(&g, NULL, NULL) == -EINVAL);

  /* from a running GAP9 with a job in flight and one waiting */
  g9_submit(&g, &j, done_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  sim_poll(true, false, &fj, &bad, 0);
  CHECK(power_ups == 1 && power_slot[0] == 0);
  slot_req = 1; /* gap9_link_reboot(1, ...) stores the slot, then asks the core */
  CHECK(g9_reboot(&g, reboot_cb, &tag) == 0);
  CHECK(g9_reboot(&g, reboot_cb, &tag) == -EBUSY);
  CHECK(!powered && ngot == 2 && gots[0].err == -ECANCELED && gots[1].err == -ECANCELED);
  CHECK(timer_ms == cfg.restart_off_ms && g9_busy(&g) && rbk.n == 0);
  g9_timer_expired(&g);
  CHECK(powered && power_ups == 2 && power_slot[1] == 1 && timer_ms == cfg.boot_ms);
  CHECK(g.tx_len == G9L_HDR); /* only NONE during boot */
  sim_hello();
  CHECK(rbk.n == 1 && rbk.err == 0 && rbk.has_hello && rbk.major == TEST_GAP9_MAJOR && rbk.user == &tag);
  CHECK(powered && !g.reboot_fn);

  /* GAP9 stays on for the grace time, and a job submitted in it needs no new boot */
  sensei_spi_header_t h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && timer_ms == cfg.reboot_grace_ms && !trig);
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  CHECK(power_ups == 2 && trig);
  gap9_do_job();
  CHECK(ngot == 3 && gots[2].err == 0);
  /* the next idle time is the normal one */
  h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered && !g9_busy(&g));

  /* from GAP9 off: the off time first, then the boot; a job submitted meanwhile runs on the new boot */
  reset();
  slot_req = 1;
  CHECK(g9_reboot(&g, reboot_cb, NULL) == 0);
  CHECK(!powered && timer_ms == cfg.restart_off_ms);
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  g9_timer_expired(&g);
  CHECK(powered && power_slot[0] == 1);
  sim_hello();
  CHECK(rbk.n == 1 && rbk.err == 0 && has("armJ"));
  gap9_do_job();
  CHECK(ngot == 1 && gots[0].err == 0);
}

static void test_reboot_failures(void) {
  tjob j = job_forecast();

  /* no HELLO: -ETIMEDOUT, GAP9 off, no second try, nothing booted again */
  reset();
  slot_req = 1;
  g9_reboot(&g, reboot_cb, NULL);
  g9_timer_expired(&g);
  CHECK(powered);
  g9_submit(&g, &j, done_cb, NULL);
  g9_timer_expired(&g); /* boot timeout */
  CHECK(rbk.n == 1 && rbk.err == -ETIMEDOUT && !rbk.has_hello);
  CHECK(!powered && ngot == 1 && gots[0].err == -ETIMEDOUT && !g9_busy(&g) && power_ups == 1 && nerr == 1);
  /* a later job boots normally (the platform keeps the slot it was given) */
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(powered && power_ups == 2);

  /* HELLO with another major version */
  reset();
  g9_reboot(&g, reboot_cb, NULL);
  g9_timer_expired(&g);
  sim_hello_major(TEST_GAP9_MAJOR + 1);
  CHECK(rbk.n == 1 && rbk.err == -EPROTO && rbk.has_hello && rbk.major == TEST_GAP9_MAJOR + 1);
  CHECK(!powered && !g9_busy(&g));

  /* a HELLO that does not decode */
  reset();
  uint8_t junk[8] = {0xff, 0xff, 0xff, 0xff};

  g9_reboot(&g, reboot_cb, NULL);
  g9_timer_expired(&g);
  sim_push(G9L_TYPE_HELLO, 0, junk, sizeof(junk), 0);
  CHECK(rbk.n == 1 && rbk.err == -EPROTO && !rbk.has_hello && !powered);

  /* power off now and the button while the reboot is pending */
  reset();
  g9_reboot(&g, reboot_cb, NULL);
  g9_timer_expired(&g);
  g9_shutdown(&g);
  CHECK(rbk.n == 1 && rbk.err == -ECANCELED && !powered && !g9_busy(&g));
  reset();
  g9_reboot(&g, reboot_cb, NULL);
  g9_timer_expired(&g);
  g9_reset(&g);
  CHECK(!powered && rbk.n == 0 && timer_ms == cfg.restart_off_ms);
  g9_timer_expired(&g);
  CHECK(powered);
  sim_hello();
  CHECK(rbk.n == 1 && rbk.err == 0);
}

/* ---- hold ---- */

static void test_hold(void) {
  tjob j = job_forecast();
  bool bad;

  reset();
  slot_req = 1;
  g9_hold(&g, true); /* off: powers up, no job */
  CHECK(powered && power_slot[0] == 1 && g9_busy(&g) && timer_ms == cfg.boot_ms);
  sim_hello();
  sensei_spi_header_t h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g); /* the idle time passes: held */
  CHECK(powered && g9_busy(&g));
  /* a job wakes it and runs */
  g9_submit(&g, &j, done_cb, NULL);
  CHECK(trig && power_ups == 1);
  gap9_do_job();
  CHECK(ngot == 1 && gots[0].err == 0);
  h = sim_poll(true, false, NULL, &bad, 0);
  g9_timer_expired(&g);
  CHECK(powered && g9_busy(&g));
  /* release: the idle timer runs again */
  g9_hold(&g, false);
  CHECK(powered && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered && !g9_busy(&g));

  /* release while a job is in flight: no power-off before the job ends */
  reset();
  g9_hold(&g, true);
  sim_hello();
  g9_submit(&g, &j, done_cb, NULL);
  tjob fj;

  sim_poll(true, false, &fj, &bad, 0);
  g9_hold(&g, false);
  CHECK(powered && timer_ms == cfg.job_ms);
  tres r;

  res_forecast(&r, fj.seq);
  sim_result(&r, 0, 0);
  h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered);

  /* a boot that fails twice clears the hold (the bus claim of the platform goes with g9_busy) */
  reset();
  g9_hold(&g, true);
  g9_timer_expired(&g);
  CHECK(!powered && g9_busy(&g)); /* one retry: the cycle */
  g9_timer_expired(&g);
  CHECK(powered);
  g9_timer_expired(&g);
  CHECK(!powered && !g9_busy(&g) && !g.hold);

  /* hold on a GAP9 that runs already: no new boot; shutdown clears it */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  g9_hold(&g, true);
  CHECK(power_ups == 1);
  g9_shutdown(&g);
  CHECK(!powered && !g9_busy(&g));
  g9_hold(&g, false); /* idempotent */
  CHECK(!powered);
}

/* ---- recovery: the HELLO of the boot loader, a second codec, the latch ---- */

typedef struct {
  uint32_t seq;
  uint8_t op;
  uint8_t data[64];
} rjob;
typedef struct {
  uint32_t seq;
  uint8_t status;
  uint8_t data[16];
} rres;

static int32_t r_encode(uint8_t *buf, size_t cap, const void *job) {
  if (cap < sizeof(rjob)) {
    return -1;
  }
  memcpy(buf, job, sizeof(rjob));
  return (int32_t)sizeof(rjob);
}
static bool r_decode_result(const uint8_t *buf, size_t len, void *res) {
  if (len != sizeof(rres)) {
    return false;
  }
  memcpy(res, buf, len);
  return true;
}
static uint32_t r_job_seq(const void *j) { return ((const rjob *)j)->seq; }
static void r_job_set_seq(void *j, uint32_t seq) { ((rjob *)j)->seq = seq; }
static uint32_t r_result_seq(const void *r) { return ((const rres *)r)->seq; }
static int r_result_err(const void *r) { return ((const rres *)r)->status == 0 ? 0 : -EIO; }
static bool r_uses_bus(const void *j) {
  (void)j;
  return true; /* never asked for a recovery job */
}

static const struct g9_codec rec_codec = {
    .job_size = sizeof(rjob),
    .result_size = sizeof(rres),
    .encode_job = r_encode,
    .decode_result = r_decode_result,
    .job_seq = r_job_seq,
    .job_set_seq = r_job_set_seq,
    .result_seq = r_result_seq,
    .result_err = r_result_err,
    .job_uses_bus = r_uses_bus,
    .gap9_major = G9L_RECOVERY_PROTO,
};

static void sim_hello_rec(uint32_t proto, uint32_t reason) {
  sensei_Version v = {G9L_RECOVERY_MAJOR, proto, reason, 0x5b};
  uint8_t b[32];
  int32_t n = sensei_pb_encode(b, sizeof(b), &sensei_Version_msg, &v);

  sim_push(G9L_TYPE_HELLO, 0, b, (size_t)n, 0);
}

static void rec_reset(void) {
  reset();
  CHECK(g9_set_recovery(&g, &rec_codec) == 0);
}

static void rdone_cb(int err, const void *res, bool final, void *user) {
  const rres *r = res;
  struct got *x = &gots[ngot++];

  (void)user;
  x->final = final;
  x->err = err;
  x->which = r ? 100 + r->status : -1;
  x->seq = r ? r->seq : 0;
  ev("R");
}

/* GAP9 side of one recovery job: fetch it (the header says JOB), run, push the result. */
static uint32_t gap9_do_rec_job(void) {
  bool bad;
  sensei_spi_header_t h = sim_poll(true, false, NULL, &bad, 0);
  rjob j;
  rres r;

  CHECK(h.type == G9L_TYPE_JOB && !bad);
  CHECK(g.tx_len == G9L_HDR + sizeof(rjob));
  memcpy(&j, g.tx + G9L_HDR, sizeof(j));
  memset(&r, 0, sizeof(r));
  r.seq = j.seq;
  sim_push(G9L_TYPE_RESULT, 0, (const uint8_t *)&r, sizeof(r), 0);
  return j.seq;
}

static void test_recovery_hello(void) {
  tjob j = job_forecast();
  tjob j2 = job_forecast();
  rjob rj;
  uint32_t reason = 0;
  bool bad;

  /* an app job boots GAP9, the boot loader answers: the job fails with ENOEXEC, GAP9 stays on, the latch is set */
  rec_reset();
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0);
  CHECK(g9_submit(&g, &j2, done_cb, NULL) == 0);
  CHECK(!g9_recovery(&g, NULL));
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(ngot == 2 && gots[0].err == -ENOEXEC && gots[1].err == -ENOEXEC && gots[0].final && nerr == 0);
  CHECK(g9_recovery(&g, &reason) && reason == G9L_RECOVERY_BOTH_BAD);
  CHECK(powered && power_ups == 1 && g.state == G9_RUN && g.version.major == G9L_RECOVERY_MAJOR);
  /* new app jobs fail at once, GAP9 is not touched */
  CHECK(g9_submit(&g, &j, done_cb, NULL) == -ENOEXEC);
  CHECK(power_ups == 1 && powered);
  /* nothing to do: NONE is served and the idle timer powers GAP9 off */
  sensei_spi_header_t h = sim_poll(true, false, NULL, &bad, 0);
  CHECK(h.type == G9L_TYPE_NONE && timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered && !g9_busy(&g));
  /* latched and off: an app job still fails at once and does not power GAP9 (the rail is not cycled per job) */
  CHECK(g9_submit(&g, &j, done_cb, NULL) == -ENOEXEC);
  CHECK(!powered && power_ups == 1);
  CHECK(g9_recovery(&g, &reason) && reason == G9L_RECOVERY_BOTH_BAD);

  /* a recovery job on the powered boot loader runs; the result comes back through the recovery codec */
  rec_reset();
  g9_hold(&g, true);
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_FORCED);
  CHECK(g9_recovery(&g, &reason) && reason == G9L_RECOVERY_FORCED && ngot == 0);
  memset(&rj, 0, sizeof(rj));
  rj.op = 7;
  CHECK(g9_submit_recovery(&g, &rj, rdone_cb, NULL) == 0);
  CHECK(has("armJ") && g.timer == G9T_FETCH);
  CHECK(gap9_do_rec_job() == 1);
  CHECK(ngot == 1 && gots[0].err == 0 && gots[0].seq == 1 && gots[0].which == 100 && gots[0].final && nerr == 0);
  /* a result in the wrong language (the app's) is dropped and counted */
  g9_submit_recovery(&g, &rj, rdone_cb, NULL);
  {
    tres tr;

    res_forecast(&tr, 2);
    sim_poll(true, false, NULL, &bad, 0);
    sim_result(&tr, 0, 0);
    CHECK(ngot == 1 && nerr == 1);
  }
  g9_shutdown(&g);
  CHECK(ngot == 2 && gots[1].err == -ECANCELED);

  /* recovery job on a GAP9 that is off: it boots, the boot loader says so, the job runs */
  rec_reset();
  CHECK(g9_submit_recovery(&g, &rj, rdone_cb, NULL) == 0);
  CHECK(powered && power_ups == 1 && has("armN") && !has("armJ"));
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(has("armJ"));
  CHECK(gap9_do_rec_job() == 1 && ngot == 1 && gots[0].err == 0);

  /* the app came up instead: the recovery job fails with ENOEXEC, and an app job after it runs */
  rec_reset();
  g9_submit_recovery(&g, &rj, rdone_cb, NULL);
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello();
  CHECK(ngot == 1 && gots[0].err == -ENOEXEC && !g9_recovery(&g, NULL));
  CHECK(gap9_do_job() == 2 && ngot == 2 && gots[1].err == 0);
  /* and a recovery job is refused at once while the app runs */
  CHECK(g9_submit_recovery(&g, &rj, rdone_cb, NULL) == -ENOEXEC);

  /* without a recovery codec (an nRF5340 without the feature): the HELLO is a version mismatch, GAP9 goes off */
  reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(!powered && ngot == 1 && gots[0].err == -EPROTO && !g9_recovery(&g, NULL));
  CHECK(g9_submit_recovery(&g, &rj, rdone_cb, NULL) == -ENOTSUP);

  /* another recovery protocol: also a mismatch */
  rec_reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello_rec(G9L_RECOVERY_PROTO + 1, G9L_RECOVERY_BOTH_BAD);
  CHECK(!powered && ngot == 1 && gots[0].err == -EPROTO && !g9_recovery(&g, NULL));

  /* the codec must fit the app's buffers */
  reset();
  {
    struct g9_codec big = rec_codec;

    big.job_size = codec.job_size + 1;
    CHECK(g9_set_recovery(&g, &big) == -EINVAL);
    big = rec_codec;
    big.result_size = codec.result_size + 1;
    CHECK(g9_set_recovery(&g, &big) == -EINVAL);
    CHECK(g9_set_recovery(&g, NULL) == -EINVAL);
  }
}

static void test_recovery_reboot_and_reprobe(void) {
  tjob j = job_forecast();
  uint32_t reason = 0;
  bool bad;

  /* a reboot (a trial boot) that lands in recovery: the callback gets ENOEXEC with the HELLO */
  rec_reset();
  g9_hold(&g, true);
  sim_hello();
  CHECK(!g9_recovery(&g, NULL));
  CHECK(g9_reboot(&g, reboot_cb, NULL) == 0);
  g9_timer_expired(&g); /* the rail is off for the restart time, then it boots */
  CHECK(powered);
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(rbk.n == 1 && rbk.err == -ENOEXEC && rbk.has_hello && rbk.major == G9L_RECOVERY_MAJOR);
  CHECK(g9_recovery(&g, &reason) && reason == G9L_RECOVERY_BOTH_BAD && powered);
  /* the other boot of the same session comes up in the app again: the latch goes */
  CHECK(g9_reboot(&g, reboot_cb, NULL) == 0);
  g9_timer_expired(&g);
  sim_hello();
  CHECK(rbk.n == 2 && rbk.err == 0 && !g9_recovery(&g, NULL));
  g9_hold(&g, false);
  g9_timer_expired(&g);

  /* re-probe: latched and off; one power-up to look. Still recovery: the latch is set again and GAP9 goes off. */
  rec_reset();
  g9_submit(&g, &j, done_cb, NULL);
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  sim_poll(true, false, NULL, &bad, 0);
  g9_timer_expired(&g);
  CHECK(!powered && g9_recovery(&g, NULL) && power_ups == 1);
  g9_reprobe(&g);
  CHECK(!g9_recovery(&g, NULL) && powered && power_ups == 2);
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(g9_recovery(&g, NULL));
  sim_poll(true, false, NULL, &bad, 0);
  CHECK(timer_ms == cfg.idle_off_ms);
  g9_timer_expired(&g);
  CHECK(!powered && g9_submit(&g, &j, done_cb, NULL) == -ENOEXEC);

  /* the node was repaired meanwhile: the probe finds the app, the latch is gone, jobs run again */
  g9_reprobe(&g);
  CHECK(powered && power_ups == 3);
  sim_hello();
  CHECK(!g9_recovery(&g, NULL));
  sim_poll(true, false, NULL, &bad, 0);
  g9_timer_expired(&g);
  CHECK(!powered);
  ngot = 0;
  CHECK(g9_submit(&g, &j, done_cb, NULL) == 0 && powered && power_ups == 4);
  sim_hello();
  gap9_do_job();
  CHECK(ngot == 1 && gots[0].err == 0);

  /* a reprobe while GAP9 is on does not power anything */
  g9_reprobe(&g);
  CHECK(power_ups == 4 && powered);
  g9_shutdown(&g);

  /* GAP9 restarts by itself in the boot loader (the app image failed again): the app job in flight fails */
  rec_reset();
  g9_hold(&g, true);
  sim_hello();
  g9_submit(&g, &j, done_cb, NULL);
  {
    tjob fj;

    sim_poll(true, false, &fj, &bad, 0);
  }
  sim_hello_rec(G9L_RECOVERY_PROTO, G9L_RECOVERY_BOTH_BAD);
  CHECK(ngot == 1 && gots[0].err == -ENOEXEC && g9_recovery(&g, NULL));
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
  test_lost_poll_recovers(0);
  test_lost_poll_recovers(1);
  test_foreign_result_ignored();
  test_submit_keeps_fetch_timer();
  test_gap9_reset_in_run();
  test_peek_keeps_none();
  test_early_error_frees_bus();
  test_version_mismatch();
  test_reset_button();
  test_reboot();
  test_reboot_failures();
  test_hold();
  test_recovery_hello();
  test_recovery_reboot_and_reprobe();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
