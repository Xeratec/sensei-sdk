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
  CHECK(strcmp(f.log, "S") == 0 && out.sent && !out.answered && lpwan_rai_send_ok(OFF, 8000, &out));

  /* LAST: option, send, no wait */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, LAST, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R2S") == 0 && out.sent && lpwan_rai_send_ok(LAST, 8000, &out));

  /* ONE_RESP with the answer: option, send, wait, NO_DATA */
  memset(&f, 0, sizeof(f));
  f.answer = true;
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SWR1") == 0 && f.waited_ms == 8000);
  CHECK(out.sent && out.answered && lpwan_rai_send_ok(ONE, 8000, &out));

  /* ONE_RESP, the wait expires: NO_DATA all the same, the send is not ok */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SWR1") == 0);
  CHECK(out.sent && !out.answered && !lpwan_rai_send_ok(ONE, 8000, &out));

  /* wait 0 (answer not awaited): no wait, NO_DATA after the send, ok when sent */
  memset(&f, 0, sizeof(f));
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 0, &out);
  CHECK(strcmp(f.log, "R3SR1") == 0 && lpwan_rai_send_ok(ONE, 0, &out));

  /* the send fails: no wait, NO_DATA still goes out, not ok */
  memset(&f, 0, sizeof(f));
  f.send_err = -5;
  lpwan_rai_send(&io, &f, ONE, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R3SR1") == 0 && !out.sent && !lpwan_rai_send_ok(ONE, 8000, &out));
  memset(&f, 0, sizeof(f));
  f.send_err = -5;
  lpwan_rai_send(&io, &f, LAST, d, sizeof(d), 8000, &out);
  CHECK(strcmp(f.log, "R2S") == 0 && !lpwan_rai_send_ok(LAST, 8000, &out));

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
  /* no answer within the timeout: ask again */
  memset(&s, 0, sizeof(s));
  s.pending = true;
  s.asked = 100;
  CHECK(lpwan_eval_decide(&s, 100 + to - 1, valid, to, retry) == LPWAN_EVAL_WAIT);
  CHECK(lpwan_eval_decide(&s, 100 + to, valid, to, retry) == LPWAN_EVAL_ASK);
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

int main(void) {
  test_eval_decide();
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
