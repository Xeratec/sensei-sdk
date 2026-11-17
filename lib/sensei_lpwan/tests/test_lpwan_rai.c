/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* RAI policy of the LTE uplink: the flag of a chunk, the sequence of socket option, send and wait on a fake socket,
 * the answer of the server, and the classes of the connection evaluation. */

#include <stdio.h>
#include <string.h>

#include "sensei_link/frag.h"
#include "sensei_lpwan/lpwan_rai.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

#define OFF sensei_LteRai_LTE_RAI_OFF
#define LAST sensei_LteRai_LTE_RAI_LAST
#define ONE sensei_LteRai_LTE_RAI_ONE_RESP

/* A fake socket that records the calls in order: 'R' + option digit, 'S' send, 'W' wait. */
struct fake {
  char log[32];
  size_t n;
  int rai_err;  /* set_rai returns this */
  int send_err; /* send returns this */
  bool answer;  /* wait_answer returns this */
  uint32_t waited_ms;
};

static void rec(struct fake *f, char c) {
  if (f->n + 1 < sizeof(f->log)) {
    f->log[f->n++] = c;
    f->log[f->n] = '\0';
  }
}

static int f_set_rai(void *ctx, enum lpwan_rai_opt opt) {
  struct fake *f = ctx;

  rec(f, 'R');
  rec(f, (char)('0' + opt));
  return f->rai_err;
}

static int f_send(void *ctx, const uint8_t *d, size_t n) {
  struct fake *f = ctx;

  (void)d;
  (void)n;
  rec(f, 'S');
  return f->send_err;
}

static bool f_wait(void *ctx, uint32_t ms) {
  struct fake *f = ctx;

  rec(f, 'W');
  f->waited_ms = ms;
  return f->answer;
}

static const struct lpwan_rai_io io = {f_set_rai, f_send, f_wait};

static void test_flag(void) {
  CHECK(lpwan_rai_for_chunk(ONE, true) == ONE);
  CHECK(lpwan_rai_for_chunk(LAST, true) == LAST);
  CHECK(lpwan_rai_for_chunk(OFF, true) == OFF);
  /* an earlier chunk of a message never gets RAI */
  CHECK(lpwan_rai_for_chunk(ONE, false) == OFF);
  CHECK(lpwan_rai_for_chunk(LAST, false) == OFF);
  CHECK(lpwan_rai_for_chunk(OFF, false) == OFF);
  CHECK(lpwan_rai_for_chunk((sensei_LteRai)9, true) == OFF); /* a value we do not know */
}

static void test_plan(void) {
  struct lpwan_rai_plan p = lpwan_rai_plan_for(OFF);

  CHECK(p.before == LPWAN_RAI_OPT_NONE && !p.wait_answer && p.after == LPWAN_RAI_OPT_NONE);
  p = lpwan_rai_plan_for(LAST);
  CHECK(p.before == LPWAN_RAI_OPT_LAST && !p.wait_answer && p.after == LPWAN_RAI_OPT_NONE);
  p = lpwan_rai_plan_for(ONE);
  CHECK(p.before == LPWAN_RAI_OPT_ONE_RESP && p.wait_answer && p.after == LPWAN_RAI_OPT_NO_DATA);
  /* the numbers are those of RAI_NO_DATA, RAI_LAST and RAI_ONE_RESP (Zephyr, nrf_socket.h) */
  CHECK(LPWAN_RAI_OPT_NO_DATA == 1 && LPWAN_RAI_OPT_LAST == 2 && LPWAN_RAI_OPT_ONE_RESP == 3);
}

static void test_sequence(void) {
  struct lpwan_rai_out out;
  struct fake f;
  const uint8_t d[4] = {1, 2, 3, 4};

  /* no RAI: only the send */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, OFF, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "S") == 0 && out.sent && !out.answered && lpwan_rai_send_ok(&out));

  /* LAST: option, send, no wait */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, LAST, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R2S") == 0 && out.sent && lpwan_rai_send_ok(&out));

  /* ONE_RESP with the answer: option, send, wait, NO_DATA */
  memset(&f, 0, sizeof(f));
  f.answer = true;
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SWR1") == 0 && f.waited_ms == 8000);
  CHECK(out.sent && out.answered && lpwan_rai_send_ok(&out) && !lpwan_rai_unconfirmed(ONE, 8000, &out));

  /* ONE_RESP, the wait expires: NO_DATA all the same; the send is ok but unconfirmed (not failed) */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SWR1") == 0);
  CHECK(out.sent && !out.answered && lpwan_rai_send_ok(&out) && lpwan_rai_unconfirmed(ONE, 8000, &out));

  /* wait 0 (answer not awaited): no wait, NO_DATA after the send, ok when sent */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 0, &out);
  CHECK(strcmp(f.log, "R3SR1") == 0 && lpwan_rai_send_ok(&out) && !lpwan_rai_unconfirmed(ONE, 0, &out));
  /* LAST and OFF never wait: never unconfirmed */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, LAST, d, sizeof(d), 8000, &out);
  CHECK(!lpwan_rai_unconfirmed(LAST, 8000, &out) && !lpwan_rai_unconfirmed(OFF, 8000, &out));

  /* the send fails: no wait, NO_DATA still goes out, not ok */
  memset(&f, 0, sizeof(f));
  f.send_err = -5;
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SR1") == 0 && !out.sent && !lpwan_rai_send_ok(&out) &&
        !lpwan_rai_unconfirmed(ONE, 8000, &out));
  memset(&f, 0, sizeof(f));
  f.send_err = -5;
  lpwan_rai_send(&io, &f, LAST, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R2S") == 0 && !lpwan_rai_send_ok(&out));

  /* a modem that refuses the option (no RAI support): counted, the send and the wait go on */
  memset(&f, 0, sizeof(f));
  f.rai_err = -22;
  f.answer = true;
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SWR1") == 0 && out.rai_errors == 2 && out.sent && out.answered);
}

static void test_answer(void) {
  uint8_t ack[SENSEI_FRAG_ACK_SIZE];
  uint8_t chunk[16];
  const uint8_t msg[5] = {9, 8, 7, 6, 5};
  size_t n;

  sensei_frag_ack(0x21, 3, ack);
  CHECK(lpwan_answer_classify(ack, sizeof(ack), true, 0x21, 3) == LPWAN_ANSWER_ACK);
  CHECK(lpwan_answer_classify(ack, sizeof(ack), true, 0x21, 2) == LPWAN_ANSWER_OLD_ACK);
  CHECK(lpwan_answer_classify(ack, sizeof(ack), true, 0x22, 3) == LPWAN_ANSWER_OLD_ACK);
  CHECK(lpwan_answer_classify(ack, sizeof(ack), false, 0x21, 3) == LPWAN_ANSWER_OLD_ACK);
  /* a downlink chunk (count >= 1) is a downlink, also with the ids of the chunk we sent */
  n = sensei_frag_chunk(0x21, msg, sizeof(msg), 1024, 0, chunk, sizeof(chunk));
  CHECK(n == 8 && lpwan_answer_classify(chunk, n, true, 0x21, 0) == LPWAN_ANSWER_DOWNLINK);
  /* an empty message is a 3-byte chunk with count 1: a downlink, not an ack */
  n = sensei_frag_chunk(0x21, msg, 0, 1024, 0, chunk, sizeof(chunk));
  CHECK(n == 3 && lpwan_answer_classify(chunk, n, true, 0x21, 0) == LPWAN_ANSWER_DOWNLINK);
  /* anything that is not exactly 3 bytes with count 0 */
  CHECK(lpwan_answer_classify(ack, 2, true, 0x21, 3) == LPWAN_ANSWER_DOWNLINK);
  CHECK(lpwan_answer_classify(ack, 0, true, 0x21, 3) == LPWAN_ANSWER_DOWNLINK);
}

static void test_eval(void) {
  CHECK(lpwan_energy_from_modem(5) == sensei_EnergyEstimate_ENERGY_EXCESSIVE);
  CHECK(lpwan_energy_from_modem(6) == sensei_EnergyEstimate_ENERGY_INCREASED);
  CHECK(lpwan_energy_from_modem(7) == sensei_EnergyEstimate_ENERGY_NORMAL);
  CHECK(lpwan_energy_from_modem(8) == sensei_EnergyEstimate_ENERGY_REDUCED);
  CHECK(lpwan_energy_from_modem(9) == sensei_EnergyEstimate_ENERGY_EFFICIENT);
  CHECK(lpwan_energy_from_modem(4) == sensei_EnergyEstimate_ENERGY_UNKNOWN);
  CHECK(lpwan_energy_from_modem(10) == sensei_EnergyEstimate_ENERGY_UNKNOWN);
  CHECK(lpwan_energy_from_modem(-1) == sensei_EnergyEstimate_ENERGY_UNKNOWN);
  CHECK(lpwan_energy_is_bad(sensei_EnergyEstimate_ENERGY_EXCESSIVE));
  CHECK(!lpwan_energy_is_bad(sensei_EnergyEstimate_ENERGY_INCREASED));
  CHECK(!lpwan_energy_is_bad(sensei_EnergyEstimate_ENERGY_UNKNOWN));
  CHECK(!lpwan_energy_is_bad(sensei_EnergyEstimate_ENERGY_NORMAL));
  /* RSRP and RSRQ indexes of the modem (AT command reference): -1 is -141 dBm, 1 is -140 dBm, 97 is -44 dBm */
  CHECK(lpwan_rsrp_dbm(-17) == -157 && lpwan_rsrp_dbm(-1) == -141 && lpwan_rsrp_dbm(1) == -140);
  CHECK(lpwan_rsrp_dbm(60) == -81 && lpwan_rsrp_dbm(97) == -44);
  CHECK(lpwan_rsrq_db(-30) == -34 && lpwan_rsrq_db(-1) == -20 && lpwan_rsrq_db(1) == -19);
  CHECK(lpwan_rsrq_db(34) == -3 && lpwan_rsrq_db(35) == -3 && lpwan_rsrq_db(46) == 2);
  CHECK(lpwan_rai_support(false, false) == sensei_RaiSupport_RAI_NONE);
  CHECK(lpwan_rai_support(true, false) == sensei_RaiSupport_RAI_AS);
  CHECK(lpwan_rai_support(false, true) == sensei_RaiSupport_RAI_CP);
  CHECK(lpwan_rai_support(true, true) == sensei_RaiSupport_RAI_AS_CP);
}

static void test_eval_decide(void) {
  struct lpwan_eval_state s;
  const uint32_t valid = 600000, to = 30000, retry = 3600000;

  memset(&s, 0, sizeof(s));
  /* nothing known: ask; then wait for the event; the event settles it */
  CHECK(lpwan_eval_decide(&s, 1000, valid, to, retry) == LPWAN_EVAL_ASK);
  s.pending = true;
  s.asked = 1000;
  CHECK(lpwan_eval_decide(&s, 5000, valid, to, retry) == LPWAN_EVAL_WAIT);
  lpwan_eval_result(&s, 6000, true, false);
  CHECK(!s.pending && s.have && lpwan_eval_decide(&s, 7000, valid, to, retry) == LPWAN_EVAL_OK);
  /* a good result expires: ask again */
  CHECK(lpwan_eval_decide(&s, 6000 + valid - 1, valid, to, retry) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 6000 + valid, valid, to, retry) == LPWAN_EVAL_ASK);
  /* a bad one defers until it expires */
  lpwan_eval_result(&s, 10000, true, true);
  CHECK(lpwan_eval_decide(&s, 10000 + valid - 1, valid, to, retry) == LPWAN_EVAL_BAD);
  CHECK(lpwan_eval_decide(&s, 10000 + valid, valid, to, retry) == LPWAN_EVAL_ASK);
  /* no answer within the timeout: a failed evaluation (send ahead), asked again after the retry time */
  memset(&s, 0, sizeof(s));
  s.pending = true;
  s.asked = 100;
  CHECK(lpwan_eval_decide(&s, 100 + to - 1, valid, to, retry) == LPWAN_EVAL_WAIT);
  CHECK(lpwan_eval_decide(&s, 100 + to, valid, to, retry) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 100 + to + retry - 1, valid, to, retry) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 100 + to + retry, valid, to, retry) == LPWAN_EVAL_ASK);
  /* a late event after the timeout still counts */
  lpwan_eval_result(&s, 100 + to + 5000, true, true);
  CHECK(lpwan_eval_decide(&s, 100 + to + 6000, valid, to, retry) == LPWAN_EVAL_BAD);
  /* an older shield (request refused as unsupported): the long hold, not the retry time */
  memset(&s, 0, sizeof(s));
  lpwan_eval_unsupported(&s, 500);
  CHECK(s.failed && s.unsupported && !s.pending);
  CHECK(lpwan_eval_decide(&s, 500 + 2 * 60000, valid, to, 60000) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 500 + LPWAN_EVAL_UNSUPPORTED_MS - 1, valid, to, 60000) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 500 + LPWAN_EVAL_UNSUPPORTED_MS, valid, to, 60000) == LPWAN_EVAL_ASK);
  /* a plain failure after that is back to the retry time */
  lpwan_eval_result(&s, 600, false, false);
  CHECK(!s.unsupported && lpwan_eval_decide(&s, 600 + 60000, valid, to, 60000) == LPWAN_EVAL_ASK);
  memset(&s, 0, sizeof(s));
  /* a failed evaluation (no result, an older shield): send ahead, do not ask again for the retry time */
  lpwan_eval_result(&s, 200, false, false);
  CHECK(!s.pending && !s.have && s.failed);
  CHECK(lpwan_eval_decide(&s, 200 + retry - 1, valid, to, retry) == LPWAN_EVAL_OK);
  CHECK(lpwan_eval_decide(&s, 200 + retry, valid, to, retry) == LPWAN_EVAL_ASK);
  /* a failure after a good result keeps that result until it expires, then the retry time rules */
  lpwan_eval_result(&s, 1000, true, true);
  lpwan_eval_result(&s, 2000, false, false);
  CHECK(s.have && s.bad && lpwan_eval_decide(&s, 3000, valid, to, retry) == LPWAN_EVAL_BAD);
  CHECK(lpwan_eval_decide(&s, 1000 + valid + 1, valid, to, retry) == LPWAN_EVAL_OK);
  /* the clock wraps at 2^32 ms */
  memset(&s, 0, sizeof(s));
  lpwan_eval_result(&s, 0xFFFFFF00u, true, true);
  CHECK(lpwan_eval_decide(&s, 0x100u, valid, to, retry) == LPWAN_EVAL_BAD);
}

/* r8 (M4): the event never comes. The scheduler checks every 35 s, a request is asked once per retry time and the data
 * go in between. Before: 1000 asks, no send. */
static void test_eval_lost_event(void) {
  struct lpwan_eval_state s;
  uint32_t now = 1000, asks = 0, oks = 0;

  memset(&s, 0, sizeof(s));
  for (int i = 0; i < 1000; i++, now += 35000) {
    enum lpwan_eval_action a = lpwan_eval_decide(&s, now, 600000, 30000, 3600000);

    if (a == LPWAN_EVAL_ASK) {
      s.pending = true;
      s.asked = now;
      asks++;
    } else if (a == LPWAN_EVAL_OK) {
      oks++;
    }
  }
  /* 35000 s in all: one ask per about 3630 s */
  CHECK(oks > 900 && asks >= 9 && asks <= 11);
}

/* The receive path while a send waits (what rx_main of the shield does with each datagram): a script of datagrams
 * arrives during the wait. */
struct rx_script {
  struct fake f; /* first: the fake socket calls get this struct as their context */
  const uint8_t *dg[4];
  size_t len[4];
  size_t n;
  uint32_t key;     /* key of the chunk that waits */
  unsigned to_node; /* downlinks passed on */
};

static bool rx_wait(void *ctx, uint32_t ms) {
  struct rx_script *r = ctx;
  bool answered = false;

  (void)ms;
  for (size_t i = 0; i < r->n; i++) {
    struct lpwan_rx x = lpwan_rx_decide(r->dg[i], r->len[i], r->key);

    r->to_node += x.downlink ? 1 : 0;
    answered = answered || x.answer; /* the semaphore: the first answer ends the wait */
    if (answered) {
      break;
    }
  }
  r->key = 0; /* wait over: nothing waits any more */
  return answered;
}

static void test_wait_rx(void) {
  struct lpwan_rai_out out;
  struct rx_script r;
  const uint8_t chunk[6] = {0x21, 3, 0xAA, 0xBB, 0xCC, 0xDD}; /* msg 0x21, chunk 3 */
  uint8_t ack_ok[SENSEI_FRAG_ACK_SIZE], ack_old[SENSEI_FRAG_ACK_SIZE], ack_other[SENSEI_FRAG_ACK_SIZE];
  uint8_t dl[8];
  const uint8_t body[5] = {9, 8, 7, 6, 5};
  size_t dln = sensei_frag_chunk(0x40, body, sizeof(body), 1024, 0, dl, sizeof(dl));
  struct lpwan_rai_io rio = {f_set_rai, f_send, rx_wait};

  sensei_frag_ack(0x21, 3, ack_ok);
  sensei_frag_ack(0x21, 2, ack_old);
  sensei_frag_ack(0x55, 3, ack_other);
  CHECK(lpwan_answer_key(chunk, sizeof(chunk)) == ((1u << 16) | 0x2103u));
  CHECK(lpwan_answer_key(chunk, 1) == 0);

  /* the ack arrives: answered, nothing for the node */
  memset(&r, 0, sizeof(r));
  r.key = lpwan_answer_key(chunk, sizeof(chunk));
  r.dg[0] = ack_ok, r.len[0] = sizeof(ack_ok), r.n = 1;
  lpwan_rai_send(&rio, &r, ONE, chunk, sizeof(chunk), 8000, &out);
  CHECK(out.answered && r.to_node == 0);

  /* a late ack of an earlier chunk and an ack of another message come first: dropped, the wait goes on, then the
   * right ack ends it */
  memset(&r, 0, sizeof(r));
  r.key = lpwan_answer_key(chunk, sizeof(chunk));
  r.dg[0] = ack_old, r.len[0] = sizeof(ack_old);
  r.dg[1] = ack_other, r.len[1] = sizeof(ack_other);
  r.dg[2] = ack_ok, r.len[2] = sizeof(ack_ok), r.n = 3;
  lpwan_rai_send(&rio, &r, ONE, chunk, sizeof(chunk), 8000, &out);
  CHECK(out.answered && r.to_node == 0);

  /* only stale acks: no answer within the wait: sent, unconfirmed */
  memset(&r, 0, sizeof(r));
  r.key = lpwan_answer_key(chunk, sizeof(chunk));
  r.dg[0] = ack_old, r.len[0] = sizeof(ack_old);
  r.dg[1] = ack_other, r.len[1] = sizeof(ack_other), r.n = 2;
  lpwan_rai_send(&rio, &r, ONE, chunk, sizeof(chunk), 8000, &out);
  CHECK(!out.answered && lpwan_rai_unconfirmed(ONE, 8000, &out) && lpwan_rai_send_ok(&out));

  /* a downlink during the wait goes to the node and answers (a stale downlink is indistinguishable) */
  memset(&r, 0, sizeof(r));
  r.key = lpwan_answer_key(chunk, sizeof(chunk));
  r.dg[0] = ack_old, r.len[0] = sizeof(ack_old);
  r.dg[1] = dl, r.len[1] = dln, r.n = 2;
  lpwan_rai_send(&rio, &r, ONE, chunk, sizeof(chunk), 8000, &out);
  CHECK(out.answered && r.to_node == 1);

  /* the right ack comes after the wait (key 0): dropped, the downlink after it is passed on, no answer */
  CHECK(!lpwan_rx_decide(ack_ok, sizeof(ack_ok), 0).answer && !lpwan_rx_decide(ack_ok, sizeof(ack_ok), 0).downlink);
  CHECK(lpwan_rx_decide(dl, dln, 0).downlink && !lpwan_rx_decide(dl, dln, 0).answer);
}

int main(void) {
  test_eval_decide();
  test_eval_lost_event();
  test_wait_rx();
  test_flag();
  test_plan();
  test_sequence();
  test_answer();
  test_eval();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("lpwan_rai: ok\n");
  return 0;
}
