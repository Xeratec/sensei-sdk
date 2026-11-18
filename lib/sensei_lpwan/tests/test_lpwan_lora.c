/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* LoRa payload limit from the data rate (EU868), airtime, send time estimate and limits, the planning of a send (what
 * the shield answers before it queues anything), the send tracking of both sides, and a model of the LR1121 (duty
 * cycle, NbTrans, repeats of a confirmed uplink) with the shield and the client around it. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/lpwan_lora.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

/* ---- Fake LR1121 ----
 * EU868 sub-band g1 allows 1 % airtime: after a frame of airtime T the LR1121 keeps the next one back until 100 T after
 * its start (the Modem-E firmware queues it, get_duty_cycle_status() reports the wait as a negative number). It sends
 * NbTrans times (+ 1 if confirmed and nobody acks), each followed by the receive windows (RX1 at 1 s, RX2 at 2 s and
 * the window itself); TX_DONE comes after the last. `slow_pct` stretches the time of a frame (the LR1121 is slower
 * than the estimate), `hang` keeps it for ever. */
#define FAKE_RX_MS 2100u

struct fake_lr {
  uint32_t now_ms;
  uint32_t blocked_until_ms;
  uint8_t dr;
  uint8_t nb_trans;
  uint8_t max_payload; /* what get_next_tx_max_payload says; 0 is not known */
  unsigned slow_pct;
  bool hang;
  /* the frame it holds */
  bool has;
  bool flush;
  uint32_t seq;
  uint32_t done_at_ms;
  unsigned frames;  /* frames put in the queue */
  unsigned flushes; /* of them empty uplinks for the MAC answers */
  unsigned resets;
};

static int32_t fake_duty_status(const struct fake_lr *f) {
  return f->now_ms >= f->blocked_until_ms ? 0 : -(int32_t)(f->blocked_until_ms - f->now_ms);
}

/* Queue an uplink of `app_len` bytes now. Returns the time at which TX_DONE comes. */
static uint32_t fake_request_tx(struct fake_lr *f, uint32_t app_len, bool confirmed) {
  uint32_t n = f->nb_trans + (confirmed ? 1u : 0u);
  uint32_t air = lpwan_lora_airtime_ms(f->dr, app_len + 13u); /* what the real frame has: no FOpts */
  uint32_t start = f->now_ms > f->blocked_until_ms ? f->now_ms : f->blocked_until_ms;

  for (uint32_t k = 0; k < n; k++) {
    if (k > 0) {
      uint32_t after_rx = start + air + FAKE_RX_MS;

      start = after_rx > f->blocked_until_ms ? after_rx : f->blocked_until_ms;
    }
    f->blocked_until_ms = start + 100u * air;
  }
  return f->now_ms + (start + air + FAKE_RX_MS - f->now_ms) * f->slow_pct / 100u;
}

/* A frame goes into the queue. */
static void fake_queue(struct fake_lr *f, uint32_t seq, uint32_t app_len, bool confirmed, bool flush) {
  f->has = true;
  f->seq = seq;
  f->flush = flush;
  f->frames++;
  f->flushes += flush;
  f->done_at_ms = fake_request_tx(f, app_len, confirmed);
}

/*
 * A message of `msg_len` bytes in chunks of the data rate's size, sent one chunk after the other (the next goes out
 * when the previous tx_done is in), as the node's uplink scheduler does. For every chunk the nRF9151 reads the wait,
 * NbTrans and the chunk limit from the LR1121 and plans the send; its watchdog and the client's deadline follow from
 * the estimate. Returns the number of chunks the old fixed 45 s watchdog would have failed.
 */
static unsigned model_message(uint8_t dr, bool adr, uint8_t nb, bool confirmed, uint32_t msg_len,
                              unsigned chunks_expected) {
  /* with ADR the estimate is for DR0, the real rate is higher: only "never early" holds */
  struct fake_lr f = {.dr = dr, .nb_trans = nb, .slow_pct = 100};
  uint32_t chunk = lpwan_lora_chunk_max(dr, adr, 0);
  uint32_t left = msg_len;
  unsigned n = 0, old_watchdog_fails = 0;

  while (left > 0) {
    uint32_t len = left < chunk ? left : chunk;
    struct lpwan_lora_send_in in = {.joined = true, .len = len};
    struct lpwan_lora_send_out out;

    in.est.data_rate = dr;
    in.est.adr = adr;
    in.est.confirmed = confirmed;
    in.est.duty_known = in.est.nb_known = true;
    in.est.duty = fake_duty_status(&f);
    in.est.nb_trans = nb;
    lpwan_lora_plan_send(&in, &out);
    CHECK(out.result == sensei_LpwanResult_LPWAN_OK);
    {
      uint32_t expected = out.expected_ms;
      uint32_t shield_limit = lpwan_send_limit_ms(expected, 45000, 5000);
      uint32_t client_limit = lpwan_send_limit_ms(expected, 60000, 20000);
      uint32_t t_send = f.now_ms;
      uint32_t actual = fake_request_tx(&f, len, confirmed) - t_send;

      CHECK(expected >= actual || expected == LPWAN_SEND_MAX_MS); /* never early: the watchdog would fail a good send */
      CHECK(adr || nb > 1 || confirmed || expected - actual <= 2500); /* and, for one frame, not much later */
      CHECK(shield_limit >= actual || expected == LPWAN_SEND_MAX_MS); /* the nRF9151 waits for the real result */
      CHECK(client_limit >= shield_limit + 15000);                    /* and ends the send first */
      CHECK(client_limit <= LPWAN_SEND_MAX_MS + 20000);
      old_watchdog_fails += actual > 45000;
      f.now_ms = t_send + actual; /* tx_done is in: the next chunk */
    }
    left -= len;
    n++;
  }
  CHECK(n == chunks_expected);
  return old_watchdog_fails;
}

static void test_duty_cycle_model(void) {
  /* a sealed status of 76 B at DR0 (51 B chunks) and a 160 B message at DR3 (115 B chunks): two chunks each. The
   * second waits for the duty cycle for minutes at DR0 and a minute at DR3; the old 45 s watchdog failed it. */
  CHECK(model_message(0, false, 1, false, 76, 2) == 1);
  CHECK(model_message(3, false, 1, false, 160, 2) == 1);
  /* three chunks at DR0: two of them wait */
  CHECK(model_message(0, false, 1, false, 140, 3) == 2);
  /* with ADR the chunks are 51 B whatever the rate, the estimate uses DR0: never early */
  CHECK(model_message(3, true, 1, false, 76, 2) == 0); /* the real DR3 airtime is short: the 45 s watchdog is not hit */
  /* DR5: one chunk of 222 B, no waiting, nothing to fail */
  CHECK(model_message(5, false, 1, false, 222, 1) == 0);
  /* DR4 and up: a 300 B message is two chunks, the wait is a few seconds */
  CHECK(model_message(5, false, 1, false, 300, 2) == 0);
  /* NbTrans 2 and 3, and a confirmed uplink (one more repeat): the estimate follows */
  CHECK(model_message(3, false, 2, false, 160, 2) > 0);
  CHECK(model_message(3, false, 3, false, 160, 2) > 0);
  /* DR5, NbTrans 3 and one more for a confirmed uplink: the repeats wait 20 s each, the old watchdog failed it */
  CHECK(model_message(5, false, 3, true, 300, 2) > 0);

  /* the wait the LR1121 reports is what the second chunk of DR0 needs: about 99 times the first airtime */
  {
    struct fake_lr f = {.dr = 0, .nb_trans = 1, .slow_pct = 100};
    uint32_t done = fake_request_tx(&f, 51, false);

    f.now_ms = done;
    CHECK(fake_duty_status(&f) < -265000 && fake_duty_status(&f) > -280000); /* 99 x 2.79 s, minus the time since */
  }
}

static void test_airtime(void) {
  /* 64 B frame (51 B payload plus 13 B LoRaWAN): known values for EU868, 125 kHz */
  CHECK(lpwan_lora_airtime_ms(0, 64) == 2794); /* SF12: 2793.5 ms */
  CHECK(lpwan_lora_airtime_ms(1, 64) == 1561);
  CHECK(lpwan_lora_airtime_ms(5, 64) >= 118 && lpwan_lora_airtime_ms(5, 64) <= 119);
  CHECK(lpwan_lora_airtime_ms(6, 64) >= 59 && lpwan_lora_airtime_ms(6, 64) <= 60); /* SF7, 250 kHz */
  CHECK(lpwan_lora_airtime_ms(7, 64) == 12);                                       /* FSK 50 kbit/s */
  /* longer frames take longer, a lower rate takes longer */
  for (unsigned dr = 0; dr < 6; dr++) {
    CHECK(lpwan_lora_airtime_ms((uint8_t)dr, 100) > lpwan_lora_airtime_ms((uint8_t)dr, 20));
    CHECK(lpwan_lora_airtime_ms((uint8_t)dr, 50) > lpwan_lora_airtime_ms((uint8_t)dr + 1, 50));
  }
  CHECK(lpwan_lora_airtime_ms(9, 64) == lpwan_lora_airtime_ms(0, 64)); /* not an EU868 rate: as DR0 */
  CHECK(lpwan_lora_airtime_ms(0, 0) > 0);
}

static void test_chunk_and_limits(void) {
  /* the LR1121 reports nothing, or room for a full chunk: the nominal size */
  CHECK(lpwan_lora_chunk_max(0, false, 0) == 51);
  CHECK(lpwan_lora_chunk_max(0, false, 51) == 51);
  CHECK(lpwan_lora_chunk_max(3, false, 222) == 115);
  /* MAC answers pending (FOpts): what is left, minus a margin for more */
  CHECK(lpwan_lora_chunk_max(0, false, 36) == 36 - LPWAN_LORA_MAC_MARGIN);
  CHECK(lpwan_lora_chunk_max(3, false, 100) == 100 - LPWAN_LORA_MAC_MARGIN);
  CHECK(lpwan_lora_chunk_max(5, true, 40) == 25); /* ADR: nominal 51 */
  CHECK(lpwan_lora_chunk_max(0, false, 5) == 4);  /* the smallest chunk the fragmenter takes */
  /* the chunk is never above the nominal value */
  for (unsigned nm = 0; nm < 256; nm++) {
    CHECK(lpwan_lora_chunk_max(0, false, nm) <= 51 && lpwan_lora_chunk_max(0, false, nm) >= 4);
  }

  /* a refused chunk: chunk again only if the size to use is smaller */
  CHECK(lpwan_lora_chunk_too_big(0, false, 36, 51) && !lpwan_lora_chunk_too_big(0, false, 0, 51));
  CHECK(!lpwan_lora_chunk_too_big(0, false, 51, 40) && !lpwan_lora_chunk_too_big(3, false, 222, 115));
  CHECK(lpwan_lora_chunk_too_big(0, false, 6, 8)); /* the size to use is 4 B */

  /* expected time: wait + transmissions + receive windows, capped */
  CHECK(lpwan_lora_expected_ms(5, false, 10, 0, 1, false) >= LPWAN_LORA_RX_MS);
  CHECK(lpwan_lora_expected_ms(5, false, 10, 1000, 1, false) ==
        lpwan_lora_expected_ms(5, false, 10, 0, 1, false) + 1000);
  CHECK(lpwan_lora_expected_ms(5, false, 10, 4000000, 1, false) == LPWAN_SEND_MAX_MS);
  CHECK(lpwan_lora_expected_ms(5, true, 10, 0, 1, false) ==
        lpwan_lora_expected_ms(0, false, 10, 0, 1, false)); /* ADR: as DR0 */
  /* repeats: NbTrans transmissions, one more if confirmed; NbTrans 0 or above 15 is not valid: 1 */
  CHECK(lpwan_lora_tx_ms(5, false, 10, 3, false) > 2 * lpwan_lora_tx_ms(5, false, 10, 1, false));
  CHECK(lpwan_lora_tx_ms(5, false, 10, 2, true) == lpwan_lora_tx_ms(5, false, 10, 3, false));
  CHECK(lpwan_lora_tx_ms(5, false, 10, 0, false) == lpwan_lora_tx_ms(5, false, 10, 1, false));
  CHECK(lpwan_lora_tx_ms(5, false, 10, 16, false) == lpwan_lora_tx_ms(5, false, 10, 1, false));
  /* TxDone.attempts: the same count as the time estimate */
  CHECK(lpwan_lora_tx_attempts(1, false, false) == 1 && lpwan_lora_tx_attempts(3, false, false) == 3);
  CHECK(lpwan_lora_tx_attempts(3, false, true) == 3); /* no ack is asked for */
  CHECK(lpwan_lora_tx_attempts(3, true, false) == 4 && lpwan_lora_tx_attempts(1, true, false) == 2);
  CHECK(lpwan_lora_tx_attempts(3, true, true) == 1); /* acknowledged: at least one */
  CHECK(lpwan_lora_tx_attempts(0, false, false) == 1 && lpwan_lora_tx_attempts(16, false, false) == 1);
  CHECK(lpwan_lora_tx_attempts(0, true, false) == 2 && lpwan_lora_tx_attempts(15, false, false) == 15);
  /* from the TX_DONE status: an unconfirmed uplink that was not sent counts 0, whatever NbTrans is */
  CHECK(lpwan_lora_tx_done_attempts(LPWAN_LORA_TX_NOT_SENT, 3, false) == 0);
  CHECK(lpwan_lora_tx_done_attempts(LPWAN_LORA_TX_NOT_SENT, 0, false) == 0);
  CHECK(lpwan_lora_tx_done_attempts(LPWAN_LORA_TX_UNCONFIRMED, 3, false) == 3);
  CHECK(lpwan_lora_tx_done_attempts(LPWAN_LORA_TX_NOT_SENT, 3, true) == 4);  /* confirmed, no ack: frames went out */
  CHECK(lpwan_lora_tx_done_attempts(LPWAN_LORA_TX_CONFIRMED, 3, true) == 1); /* acknowledged */
  for (uint32_t nb = 0; nb < LPWAN_LORA_NB_TRANS_MAX; nb++) { /* the count and the time of the estimate agree */
    CHECK(lpwan_lora_tx_ms(5, false, 10, nb, true) ==
          lpwan_lora_tx_ms(5, false, 10, lpwan_lora_tx_attempts(nb, true, false), false));
  }
  /* every repeat waits for the duty cycle: at DR0 about 100 x 2.8 s apart */
  CHECK(lpwan_lora_tx_ms(0, false, 51, 3, false) > 2 * 277000);
  CHECK(lpwan_lora_tx_ms(0, false, 51, 2, false) < LPWAN_SEND_MAX_MS); /* 2 transmissions of a full chunk fit */
  CHECK(lpwan_lora_tx_ms(0, false, 51, 3, false) > LPWAN_SEND_MAX_MS); /* 3 do not */

  /* limits: the floor without an estimate, estimate plus extra with one, never above the cap plus extra */
  CHECK(lpwan_send_limit_ms(0, 45000, 5000) == 45000);
  CHECK(lpwan_send_limit_ms(0, 60000, 20000) == 60000);
  CHECK(lpwan_send_limit_ms(100000, 45000, 5000) == 105000);
  CHECK(lpwan_send_limit_ms(100000, 60000, 20000) == 120000);
  CHECK(lpwan_send_limit_ms(UINT32_MAX, 45000, 5000) == LPWAN_SEND_MAX_MS + 5000);
}

/* ---- the estimate and the planning of a send ---- */

static struct lpwan_lora_send_in plan_in(uint32_t len) {
  struct lpwan_lora_send_in in = {.joined = true, .len = len, .max_known = true, .max = 51};

  in.est.data_rate = 0;
  in.est.duty_known = in.est.nb_known = true;
  in.est.nb_trans = 1;
  return in;
}

static void test_estimate(void) {
  struct lpwan_lora_est e, c;
  struct lpwan_lora_est_in in = {.data_rate = 5, .app_len = 20, .duty_known = true, .nb_known = true, .nb_trans = 2};

  in.duty = -12000;
  lpwan_lora_estimate(&in, &e);
  CHECK(e.wait_ms == 12000);
  CHECK(e.tx_ms == lpwan_lora_tx_ms(5, false, 20, 2, false));
  in.duty = 5000; /* a budget, no wait */
  lpwan_lora_estimate(&in, &e);
  CHECK(e.wait_ms == 0);
  in.duty = INT32_MIN;
  lpwan_lora_estimate(&in, &e);
  CHECK(e.wait_ms == 2147483648u);

  /* a failed call or a nonsense value: conservative (DR0, NbTrans 1, confirmed), never 0 */
  in.duty = -1000;
  lpwan_lora_estimate(&in, &e);
  in.duty_known = false;
  lpwan_lora_estimate(&in, &c);
  CHECK(c.tx_ms == lpwan_lora_tx_ms(0, false, 20, 2, true));                       /* a valid NbTrans is kept */
  CHECK(c.wait_ms == 99 * lpwan_lora_airtime_ms(0, 20 + LPWAN_LORA_PHY_OVERHEAD)); /* a frame of this size just went */
  CHECK(c.wait_ms + c.tx_ms > e.wait_ms + e.tx_ms);
  in.duty_known = true;
  for (unsigned bad = 0; bad < 2; bad++) {
    in.nb_known = bad == 1;
    in.nb_trans = bad == 1 ? 0 : 3;
    lpwan_lora_estimate(&in, &c);
    CHECK(c.tx_ms == lpwan_lora_tx_ms(0, false, 20, 1, true));
    CHECK(c.tx_ms >= LPWAN_LORA_RX_MS && c.wait_ms == 1000);
  }
  in.nb_known = true;
  in.nb_trans = 16;
  lpwan_lora_estimate(&in, &c);
  CHECK(c.tx_ms == lpwan_lora_tx_ms(0, false, 20, 1, true));
  in.nb_known = in.duty_known = false;
  lpwan_lora_estimate(&in, &c);
  CHECK(c.wait_ms > 0 && c.tx_ms > LPWAN_LORA_RX_MS);
}

static void test_plan_send(void) {
  struct lpwan_lora_send_in in = plan_in(40);
  struct lpwan_lora_send_out out;

  /* fits, no wait */
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.next_max == 51 && out.queued_seq == 0 && !out.flush);
  CHECK(out.expected_ms == lpwan_lora_expected_ms(0, false, 40, 0, 1, false));
  /* NbTrans and a confirmed uplink lengthen it */
  in.est.nb_trans = 3;
  in.est.confirmed = true;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.expected_ms == LPWAN_SEND_MAX_MS); /* 4 x 277 s: capped */
  in = plan_in(40);

  /* length 0 or above 222, not joined */
  in.len = 0;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_INVALID && out.next_max == 0);
  in.len = 223;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_INVALID);
  in = plan_in(40);
  in.joined = false;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_NOT_JOINED);

  /* a frame is held: BUSY with its seq and the time left; the state of the LR1121 is not asked */
  in = plan_in(40);
  in.held = true;
  in.held_seq = 17;
  in.held_left_ms = 123456;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && out.queued_seq == 17 && out.expected_ms == 123456 && !out.flush);

  /* the next uplink takes less than the chunk: INVALID with what fits; 0 is "not known", no check (F8) */
  in = plan_in(51);
  in.max = 36;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_INVALID && out.next_max == 36);
  in.max = 0;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.next_max == 0);
  in.max_known = false;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.next_max == 0);
  /* so little room that no chunk fits: MAC answers use it. BUSY, an empty uplink flushes them */
  in.max_known = true;
  in.max = LPWAN_LORA_MIN_CHUNK - 1;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && out.flush && out.queued_seq == 0 && out.next_max == in.max);
  CHECK(out.expected_ms == lpwan_lora_expected_ms(0, false, 0, 0, 1, false));
  in.max = LPWAN_LORA_MIN_CHUNK;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_INVALID && !out.flush && out.next_max == LPWAN_LORA_MIN_CHUNK);
  in.len = LPWAN_LORA_MIN_CHUNK;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK);
  /* the flush frame has to wait too: then it is not queued */
  in = plan_in(51);
  in.max = 2;
  in.est.duty = -(int32_t)(2 * LPWAN_SEND_MAX_MS);
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && !out.flush && out.expected_ms > 0);

  /* a wait that makes the send longer than the limit: refused before it is queued, with the time to wait */
  in = plan_in(51);
  in.est.duty = -(int32_t)(LPWAN_SEND_MAX_MS + 1000);
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && out.queued_seq == 0 && !out.flush);
  {
    uint32_t tx = lpwan_lora_tx_ms(0, false, 51, 1, false);

    CHECK(out.expected_ms == LPWAN_SEND_MAX_MS + 1000 - (LPWAN_SEND_MAX_MS - tx));
    /* after that time the wait is that much shorter and the send is taken */
    in.est.duty += (int32_t)out.expected_ms;
    lpwan_lora_plan_send(&in, &out);
    CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.expected_ms == LPWAN_SEND_MAX_MS);
  }
  /* a wait of 15 min (the 1 h window of 0.1 % bands) is refused too */
  in = plan_in(51);
  in.est.duty = -900000;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && out.expected_ms > 300000 && out.expected_ms < 900000);
  /* frames that alone take longer than the limit (DR0, NbTrans 3, confirmed) are taken when there is no wait */
  in = plan_in(51);
  in.est.nb_trans = 3;
  in.est.confirmed = true;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_OK && out.expected_ms == LPWAN_SEND_MAX_MS);
  in.est.duty = -1000;
  lpwan_lora_plan_send(&in, &out);
  CHECK(out.result == sensei_LpwanResult_LPWAN_BUSY && out.expected_ms == 1000); /* only the wait counts */
}

/* ---- the nRF9151 side of a send: watchdog and late results ---- */

static void test_txtrack(void) {
  struct lpwan_txtrack t;
  uint32_t seq, rearm, late_seq;
  bool late_ok;

  lpwan_txt_init(&t);
  /* a send: open, armed, result */
  lpwan_txt_open(&t, 5);
  CHECK(lpwan_txt_wants(&t, 5) && !lpwan_txt_wants(&t, 6) && !lpwan_txt_wants(&t, 0));
  CHECK(lpwan_txt_armed(&t, 5, 1000, 50000));
  CHECK(lpwan_txt_wd(&t, 51000, false, &seq, &rearm) == LPWAN_WD_FAIL && seq == 5);
  lpwan_txt_done(&t, 5);
  CHECK(!lpwan_txt_wants(&t, 5));
  /* a second result for it is late, kept once */
  lpwan_txt_late(&t, 5, true);
  CHECK(lpwan_txt_take_late(&t, &late_seq, &late_ok) && late_seq == 5 && late_ok);
  CHECK(!lpwan_txt_take_late(&t, &late_seq, &late_ok));
  lpwan_txt_late(&t, 0, true); /* no seq: not kept */
  CHECK(!lpwan_txt_take_late(&t, &late_seq, &late_ok));

  /* the result came before the backend returned: do not arm */
  lpwan_txt_open(&t, 6);
  lpwan_txt_done(&t, 6);
  CHECK(!lpwan_txt_armed(&t, 6, 0, 1000));
  /* refused: nothing open */
  lpwan_txt_open(&t, 7);
  lpwan_txt_refused(&t, 7);
  CHECK(!lpwan_txt_wants(&t, 7));

  /* F7: a watchdog of send 8 does nothing once send 9 is open */
  lpwan_txt_open(&t, 8);
  CHECK(lpwan_txt_armed(&t, 8, 0, 45000));
  lpwan_txt_done(&t, 8);
  lpwan_txt_open(&t, 9);
  CHECK(lpwan_txt_wd(&t, 46000, false, &seq, &rearm) == LPWAN_WD_NONE && lpwan_txt_wants(&t, 9)); /* not armed yet */
  CHECK(lpwan_txt_armed(&t, 9, 45500, 45000));
  /* a stale run that was waiting for the lock sees send 9 only 500 ms old: not due */
  CHECK(lpwan_txt_wd(&t, 46000, false, &seq, &rearm) == LPWAN_WD_WAIT && rearm == 44500 && seq == 9);
  CHECK(lpwan_txt_wd(&t, 45500 + 45000, false, &seq, &rearm) == LPWAN_WD_FAIL && seq == 9);
  lpwan_txt_done(&t, 9);
  CHECK(lpwan_txt_wd(&t, 200000, false, &seq, &rearm) == LPWAN_WD_NONE);

  /* the frame is still held at the limit: wait, look again at twice the limit, then reset */
  lpwan_txt_open(&t, 10);
  CHECK(lpwan_txt_armed(&t, 10, 100000, 60000));
  CHECK(lpwan_txt_wd(&t, 160000, true, &seq, &rearm) == LPWAN_WD_WAIT && rearm == 60000 && seq == 10);
  CHECK(lpwan_txt_wd(&t, 219999, true, &seq, &rearm) == LPWAN_WD_WAIT && rearm == 1);
  CHECK(lpwan_txt_wd(&t, 220000, true, &seq, &rearm) == LPWAN_WD_RESET && seq == 10);
  /* it went out meanwhile and no result event is held back: it is not held, the send fails */
  CHECK(lpwan_txt_wd(&t, 180000, false, &seq, &rearm) == LPWAN_WD_FAIL);
  /* the clock wraps (49 days): differences still work */
  lpwan_txt_done(&t, 10);
  lpwan_txt_open(&t, 11);
  CHECK(lpwan_txt_armed(&t, 11, 0xFFFFF000u, 60000));
  CHECK(lpwan_txt_wd(&t, 0xFFFFF000u + 60000u, true, &seq, &rearm) == LPWAN_WD_WAIT && rearm == 60000);
}

/* ---- the nRF5340 side: what the shield may hold ---- */

static void test_held(void) {
  struct lpwan_held h = {0};
  const uint8_t a[] = {1, 2, 3, 4, 5}, b[] = {1, 2, 3, 4, 6};
  sensei_LpwanStatus st = sensei_LpwanStatus_init_zero;

  lpwan_held_arm(&h, 40, a, sizeof(a));
  lpwan_held_expect(&h, 1000, 60000);
  CHECK(lpwan_held_keeps_power(&h, 1000) && lpwan_held_keeps_power(&h, 120999) && !lpwan_held_keeps_power(&h, 121000));
  /* BUSY with the seq of that frame: the same chunk adopts it, another one waits and sends after */
  CHECK(lpwan_held_busy(&h, a, sizeof(a), 40) == LPWAN_BUSY_ADOPT);
  CHECK(lpwan_held_busy(&h, b, sizeof(b), 40) == LPWAN_BUSY_WAIT_RESEND);
  CHECK(lpwan_held_busy(&h, a, sizeof(a) - 1, 40) == LPWAN_BUSY_WAIT_RESEND);
  CHECK(lpwan_held_busy(&h, a, sizeof(a), 41) == LPWAN_BUSY_WAIT_RESEND); /* another frame than the one we know */
  CHECK(lpwan_held_busy(&h, a, sizeof(a), 0) == LPWAN_BUSY_RETRY_LATER);
  /* a result for another seq changes nothing; for that seq it frees the frame */
  lpwan_held_result(&h, 39);
  CHECK(h.held && lpwan_held_keeps_power(&h, 1000));
  CHECK(lpwan_held_busy(&h, a, sizeof(a), 40) == LPWAN_BUSY_ADOPT);
  lpwan_held_result(&h, 40);
  CHECK(!h.held && !lpwan_held_keeps_power(&h, 1000));
  CHECK(lpwan_held_busy(&h, a, sizeof(a), 40) == LPWAN_BUSY_WAIT_RESEND); /* nothing known held: still wait */
  lpwan_held_arm(&h, 41, a, sizeof(a));
  lpwan_held_clear(&h);
  CHECK(!h.held);

  /* the deadline: ask the shield; a frame held: wait once more; a late result in the status: that is the result */
  CHECK(lpwan_held_deadline(7, false, false, &st) == LPWAN_DL_GIVE_UP);
  CHECK(lpwan_held_deadline(7, false, true, &st) == LPWAN_DL_GIVE_UP);
  st.queued = 1;
  CHECK(lpwan_held_deadline(7, false, true, &st) == LPWAN_DL_EXTEND);
  CHECK(lpwan_held_deadline(7, true, true, &st) == LPWAN_DL_GIVE_UP);
  st.late_tx_seq = 7;
  st.late_tx_ok = true;
  CHECK(lpwan_held_deadline(7, false, true, &st) == LPWAN_DL_OK);
  CHECK(lpwan_held_deadline(7, true, true, &st) == LPWAN_DL_OK);
  st.late_tx_ok = false;
  CHECK(lpwan_held_deadline(7, false, true, &st) == LPWAN_DL_FAILED);
  CHECK(lpwan_held_deadline(8, false, true, &st) == LPWAN_DL_EXTEND); /* a late result of another send */
}

/* ---- shield and client around the fake LR1121 ---- */

/* The same decisions as NRF/apps/lpwan_shield (lora.c, service.c) and lib/sensei_lpwan/zephyr/lpwan_client.c, with
 * simulated time (ms): the shield plans a send from what the fake LR1121 says and tracks it in `txt`; its watchdog
 * looks at the frame; the client arms what it sends, waits for tx_done, asks the status at its deadline and handles
 * BUSY. The Zephyr files only add the I/O around these calls. */
struct sim {
  struct fake_lr lr;
  struct lpwan_txtrack txt;
  uint32_t wd_due; /* the shield's watchdog work, UINT32_MAX: not armed */
  uint32_t seq;
  /* client */
  struct lpwan_held held, held_prev;
  bool sending;
  uint32_t wait_seq;
  bool resend_after;
  bool extended;
  uint32_t deadline, start, period, retry_at;
  uint8_t chunk[64];
  size_t chunk_len;
  bool confirmed;
  int result; /* of the last send: 0 ok, else a negative errno; 1: pending */
  unsigned resends, gave_up;
};

#define NEVER UINT32_MAX

static void sim_init(struct sim *m, uint8_t dr, uint8_t nb) {
  memset(m, 0, sizeof(*m));
  m->lr.dr = dr;
  m->lr.nb_trans = nb;
  m->lr.slow_pct = 100;
  m->lr.max_payload = 0;
  m->wd_due = NEVER;
  m->deadline = NEVER;
  lpwan_txt_init(&m->txt);
}

static uint32_t sim_left(const struct sim *m) {
  return m->lr.has && m->lr.done_at_ms > m->lr.now_ms ? m->lr.done_at_ms - m->lr.now_ms : 5000;
}

/* ---- shield ---- */

static void client_event(struct sim *m, uint32_t seq, bool ok);

static void shield_emit_tx_done(struct sim *m, uint32_t seq, bool ok) {
  if (lpwan_txt_wants(&m->txt, seq)) {
    lpwan_txt_done(&m->txt, seq);
    m->wd_due = NEVER;
    client_event(m, seq, ok);
  } else {
    lpwan_txt_late(&m->txt, seq, ok);
  }
}

struct sim_rsp {
  sensei_LpwanResult result;
  uint32_t expected_ms, next_max, queued_seq;
};

static struct sim_rsp shield_send(struct sim *m, uint32_t seq, uint32_t len, bool confirmed) {
  struct fake_lr *f = &m->lr;
  struct sim_rsp r = {0};
  struct lpwan_lora_send_in in = {.joined = true, .len = len};
  struct lpwan_lora_send_out out;

  if (m->txt.open_seq != 0) { /* a send is open (the client gave up on it): the state machine says BUSY */
    r.result = sensei_LpwanResult_LPWAN_BUSY;
    r.queued_seq = f->has && !f->flush ? f->seq : 0;
    r.expected_ms = sim_left(m);
    return r;
  }
  in.held = f->has;
  in.held_seq = f->flush ? 0 : f->seq;
  in.held_left_ms = sim_left(m);
  in.max_known = true;
  in.max = f->max_payload;
  in.est.data_rate = f->dr;
  in.est.confirmed = confirmed;
  in.est.duty_known = in.est.nb_known = true;
  in.est.duty = fake_duty_status(f);
  in.est.nb_trans = f->nb_trans;
  lpwan_lora_plan_send(&in, &out);
  r.result = out.result;
  r.expected_ms = out.expected_ms;
  r.next_max = out.next_max;
  r.queued_seq = out.queued_seq;
  if (out.result != sensei_LpwanResult_LPWAN_OK) {
    if (out.flush) {
      fake_queue(f, 0, 0, false, true);
      f->max_payload = 0; /* the MAC answers are out afterwards: the LR1121 says 0 or the full room */
    }
    return r;
  }
  lpwan_txt_open(&m->txt, seq);
  fake_queue(f, seq, len, confirmed, false);
  if (lpwan_txt_armed(&m->txt, seq, f->now_ms, lpwan_send_limit_ms(out.expected_ms, 45000, 5000))) {
    m->wd_due = f->now_ms + lpwan_send_limit_ms(out.expected_ms, 45000, 5000);
  }
  return r;
}

static void shield_wd_fire(struct sim *m) {
  struct fake_lr *f = &m->lr;
  uint32_t seq, rearm;
  bool held = f->has && !f->flush && f->seq == m->txt.open_seq;

  m->wd_due = NEVER;
  switch (lpwan_txt_wd(&m->txt, f->now_ms, held, &seq, &rearm)) {
  case LPWAN_WD_NONE:
    break;
  case LPWAN_WD_WAIT:
    m->wd_due = f->now_ms + rearm;
    break;
  case LPWAN_WD_FAIL:
    shield_emit_tx_done(m, seq, false);
    break;
  case LPWAN_WD_RESET:
    f->has = false; /* the reset drops the frame */
    f->resets++;
    shield_emit_tx_done(m, seq, false);
    break;
  }
}

/* ---- client ---- */

static void client_do_send(struct sim *m);

static void client_finish(struct sim *m, int err) {
  m->sending = false;
  m->deadline = NEVER;
  m->extended = false;
  m->resend_after = false;
  m->resends = 0;
  m->result = err;
}

static void client_arm_deadline(struct sim *m, uint32_t expected) {
  m->period = lpwan_send_limit_ms(expected, 60000, 20000);
  m->start = m->lr.now_ms;
  m->deadline = m->start + m->period;
  lpwan_held_expect(&m->held, m->lr.now_ms, m->period);
}

static void client_do_send(struct sim *m) {
  struct sim_rsp r;
  uint32_t seq = ++m->seq;

  m->wait_seq = seq;
  m->held_prev = m->held;
  lpwan_held_arm(&m->held, seq, m->chunk, m->chunk_len);
  r = shield_send(m, seq, (uint32_t)m->chunk_len, m->confirmed);
  if (r.result == sensei_LpwanResult_LPWAN_OK) {
    if (m->sending) { /* the result may be in already (not in this model) */
      client_arm_deadline(m, r.expected_ms);
    }
    return;
  }
  m->held = m->held_prev; /* refused: this chunk was not taken */
  if (r.result == sensei_LpwanResult_LPWAN_INVALID) {
    client_finish(m, lpwan_lora_chunk_too_big(m->lr.dr, false, r.next_max, (uint32_t)m->chunk_len) ? -EMSGSIZE : -EIO);
  } else if (r.result == sensei_LpwanResult_LPWAN_BUSY) {
    enum lpwan_busy_action act = lpwan_held_busy(&m->held, m->chunk, m->chunk_len, r.queued_seq);

    if (act == LPWAN_BUSY_RETRY_LATER) {
      m->retry_at = m->lr.now_ms + r.expected_ms;
      client_finish(m, -EAGAIN);
    } else if (act == LPWAN_BUSY_WAIT_RESEND && ++m->resends > 2) {
      client_finish(m, -EBUSY);
    } else {
      m->wait_seq = r.queued_seq;
      m->resend_after = act == LPWAN_BUSY_WAIT_RESEND;
      client_arm_deadline(m, r.expected_ms);
    }
  } else {
    client_finish(m, -EIO);
  }
}

static void client_event(struct sim *m, uint32_t seq, bool ok) {
  lpwan_held_result(&m->held, seq);
  if (!m->sending || seq != m->wait_seq) {
    return; /* late or another send's */
  }
  if (m->resend_after) {
    m->resend_after = false;
    m->deadline = NEVER;
    client_do_send(m);
    return;
  }
  client_finish(m, ok ? 0 : -EIO);
}

static void client_deadline_fire(struct sim *m) {
  sensei_LpwanStatus st = sensei_LpwanStatus_init_zero;
  bool ok;

  st.queued = m->lr.has ? 1 : 0;
  (void)lpwan_txt_take_late(&m->txt, &st.late_tx_seq, &st.late_tx_ok);
  switch (lpwan_held_deadline(m->wait_seq, m->extended, true, &st)) {
  case LPWAN_DL_OK:
  case LPWAN_DL_FAILED:
    ok = st.late_tx_ok;
    lpwan_held_result(&m->held, m->wait_seq);
    client_finish(m, ok ? 0 : -EIO);
    break;
  case LPWAN_DL_EXTEND:
    m->extended = true;
    m->deadline += m->deadline - m->start;
    break;
  default:
    m->gave_up++;
    client_finish(m, -ETIMEDOUT);
    break;
  }
}

/* Start a send of `len` bytes. */
static void client_send(struct sim *m, size_t len, uint8_t fill, bool confirmed) {
  memset(m->chunk, fill, sizeof(m->chunk));
  m->chunk_len = len;
  m->confirmed = confirmed;
  m->sending = true;
  m->result = 1;
  client_do_send(m);
}

/* Time passes to `t`. */
static void sim_run(struct sim *m, uint32_t t) {
  struct fake_lr *f = &m->lr;

  for (;;) {
    uint32_t t_lr = f->has && !f->hang ? f->done_at_ms : NEVER;
    uint32_t t_dl = m->sending ? m->deadline : NEVER;
    uint32_t next = t_lr;

    next = m->wd_due < next ? m->wd_due : next;
    next = t_dl < next ? t_dl : next;
    if (next > t) {
      break;
    }
    f->now_ms = next < f->now_ms ? f->now_ms : next;
    if (next == t_lr) {
      f->has = false;
      if (!f->flush) {
        shield_emit_tx_done(m, f->seq, true);
      }
    } else if (next == m->wd_due) {
      shield_wd_fire(m);
    } else {
      client_deadline_fire(m);
    }
  }
  f->now_ms = t;
}

/* The shield goes off after the idle time unless the client holds it on. Returns true if it would be powered off. */
static bool sim_idle_power_off(const struct sim *m) {
  return !m->sending && !lpwan_held_keeps_power(&m->held, m->lr.now_ms);
}

static void test_sim_normal(void) {
  struct sim m;

  /* DR0, NbTrans 3: three transmissions of a full chunk, 280 s apart: more than a send may take, taken anyway (no wait)
   */
  sim_init(&m, 0, 3);
  client_send(&m, 51, 1, false);
  CHECK(m.result == 1 && m.lr.frames == 1);
  sim_run(&m, 700000);
  CHECK(m.result == 0 && m.lr.frames == 1 && m.gave_up == 0 && m.lr.resets == 0);
  /* the nominal chunk, estimate against the real time, whatever the data rate or repeats */
  for (unsigned dr = 0; dr < 6; dr++) {
    for (unsigned nb = 1; nb <= 3; nb++) {
      for (unsigned conf = 0; conf < 2; conf++) {
        uint32_t len = lpwan_lora_max_payload((uint8_t)dr, false);
        struct lpwan_lora_send_in in = plan_in(len);
        struct lpwan_lora_send_out out;
        struct fake_lr f = {.dr = (uint8_t)dr, .nb_trans = (uint8_t)nb, .slow_pct = 100};
        uint32_t actual = fake_request_tx(&f, len, conf);

        in.est.data_rate = (uint8_t)dr;
        in.est.nb_trans = (uint8_t)nb;
        in.est.confirmed = conf;
        lpwan_lora_plan_send(&in, &out);
        if (out.result == sensei_LpwanResult_LPWAN_OK) {
          CHECK(out.expected_ms >= actual || out.expected_ms == LPWAN_SEND_MAX_MS);
        }
      }
    }
  }
}

/* The LR1121 takes longer than the estimate: the shield's watchdog fires while it holds the frame (F3). */
static void test_sim_frame_queued_past_watchdog(void) {
  struct sim m;
  uint32_t t;

  sim_init(&m, 0, 1);
  client_send(&m, 51, 1, false); /* chunk 1: no wait */
  sim_run(&m, 20000);
  CHECK(m.result == 0 && m.lr.frames == 1);
  m.lr.slow_pct = 150;
  t = m.lr.now_ms;
  client_send(&m, 51, 2,
              false); /* chunk 2 waits 270 s for the duty cycle, and the LR1121 is 50 % slower than expected */
  CHECK(m.result == 1 && m.lr.frames == 2);
  /* the watchdog (limit about 288 s) fires: the frame is held, no tx_done(failed), no reset */
  sim_run(&m, t + 295000);
  CHECK(m.result == 1 && m.txt.open_seq == m.seq && m.lr.resets == 0);
  /* the client's deadline (about 303 s) passes: the status says a frame is held: it waits once more */
  sim_run(&m, t + 310000);
  CHECK(m.result == 1 && m.extended && m.gave_up == 0);
  CHECK(!sim_idle_power_off(&m)); /* the shield stays on */
  /* the frame goes out at about 425 s: that is the result, once, and nothing was sent twice */
  sim_run(&m, t + 700000);
  CHECK(m.result == 0 && m.lr.frames == 2 && m.gave_up == 0 && m.lr.resets == 0 && !m.lr.has);
  CHECK(m.txt.open_seq == 0 && m.txt.late_seq == 0);
}

/* The LR1121 never reports: reset after twice the limit, one failure, no second frame (F3). */
static void test_sim_stuck_frame(void) {
  struct sim m;

  sim_init(&m, 3, 1);
  m.lr.hang = true;
  client_send(&m, 8, 1, false);
  sim_run(&m, 44000);
  CHECK(m.result == 1 && m.lr.resets == 0);
  sim_run(&m, 61000); /* the client's deadline (60 s): the frame is held, wait once more */
  CHECK(m.result == 1 && m.extended && m.gave_up == 0);
  CHECK(!sim_idle_power_off(&m));
  sim_run(&m, 89000);
  CHECK(m.result == 1 && m.lr.resets == 0); /* the shield's watchdog waited at 45 s */
  sim_run(&m, 91000);                       /* 2 x 45 s: reset */
  CHECK(m.result == -EIO && m.lr.resets == 1 && m.lr.frames == 1 && !m.lr.has && m.gave_up == 0);
  CHECK(m.txt.open_seq == 0);
}

/* The client gave up on a send (it timed out), the frame is still in the LR1121: the same chunk again is not sent
 * twice, another one waits for it (BUSY handled as wait). */
static void test_sim_busy_is_wait(void) {
  struct sim m;
  uint32_t t;

  for (unsigned same = 0; same < 2; same++) {
    sim_init(&m, 0, 1);
    client_send(&m, 51, 1, false);
    sim_run(&m, 10000);
    CHECK(m.result == 0);
    client_send(&m, 51, 2, false); /* chunk 2: waits 270 s */
    t = m.lr.now_ms;
    sim_run(&m, t + 5000);
    CHECK(m.result == 1 && m.lr.frames == 2);
    /* the node gave up (its own timeout): the client forgets the send but not the frame */
    m.sending = false;
    m.deadline = NEVER;
    CHECK(!sim_idle_power_off(&m)); /* the shield is not powered off while it may hold the frame */
    /* the retry: the same chunk, or another one */
    client_send(&m, 51, same ? 2 : 3, false);
    CHECK(m.result == 1 && m.sending && m.lr.frames == 2); /* nothing new was queued */
    CHECK(m.wait_seq == 2 && m.resend_after == !same);
    sim_run(&m, t + 400000);
    if (same) {
      CHECK(m.result == 0 && m.lr.frames == 2); /* the first frame is the result */
    } else {
      /* the other chunk is sent when the first is out (wait for the duty cycle again) */
      CHECK(m.result == 1 && m.lr.frames == 3);
      sim_run(&m, t + 1200000);
      CHECK(m.result == 0 && m.lr.frames == 3);
    }
    CHECK(m.gave_up == 0 && m.lr.resets == 0);
  }
}

/* The answer to a send was lost: the frame is queued, the client sends again (a new seq): it adopts the frame, and the
 * result that has no open send on the shield comes through the status. */
static void test_sim_lost_response(void) {
  struct sim m;

  sim_init(&m, 3, 1);
  /* the shield took chunk 1 (seq 1) but the client did not see the answer */
  memset(m.chunk, 7, sizeof(m.chunk));
  m.chunk_len = 8;
  m.seq = 1;
  lpwan_held_arm(&m.held, 1, m.chunk, m.chunk_len);
  fake_queue(&m.lr, 1, 8, false, false);
  /* the client retries: seq 2, the shield has nothing open but the LR1121 holds the frame */
  m.sending = true;
  m.result = 1;
  client_do_send(&m);
  CHECK(m.sending && m.wait_seq == 1 && !m.resend_after && m.lr.frames == 1);
  sim_run(&m, 100000);
  /* tx_done(1): no send waits for it on the shield, so it is kept for the status; the client's deadline reads it */
  CHECK(m.result == 0 && m.lr.frames == 1 && m.gave_up == 0);
}

/* A wait longer than a send may take: refused with the time, nothing queued; after it the send is taken (F3). */
static void test_sim_long_wait(void) {
  struct sim m;

  sim_init(&m, 3, 1);
  m.lr.blocked_until_ms = 20 * 60 * 1000; /* the LR1121 is blocked for 20 min (a 0.1 % band, a window of an hour) */
  client_send(&m, 8, 1, false);
  CHECK(m.result == -EAGAIN && m.lr.frames == 0 && !m.lr.has && m.txt.open_seq == 0);
  CHECK(m.retry_at > 10 * 60 * 1000 && m.retry_at < 20 * 60 * 1000);
  CHECK(sim_idle_power_off(&m)); /* nothing is held */
  sim_run(&m, m.retry_at);
  client_send(&m, 8, 1, false);
  CHECK(m.result == 1 && m.lr.frames == 1);
  sim_run(&m, 40 * 60 * 1000);
  CHECK(m.result == 0 && m.lr.frames == 1);
}

/* The next uplink takes less than any chunk: an empty uplink flushes the MAC answers, then the chunk goes (F8). */
static void test_sim_flush(void) {
  struct sim m;

  sim_init(&m, 3, 1);
  m.lr.max_payload = 2;
  client_send(&m, 8, 1, false);
  CHECK(m.result == -EAGAIN && m.lr.flushes == 1 && m.lr.has && m.lr.flush && m.txt.open_seq == 0);
  /* while the empty uplink is held, another send is refused the same way and is not queued again */
  client_send(&m, 8, 1, false);
  CHECK(m.result == -EAGAIN && m.lr.frames == 1);
  sim_run(&m, m.retry_at + 1000);
  CHECK(!m.lr.has);
  client_send(&m, 8, 1, false);
  CHECK(m.result == 1 && m.lr.frames == 2 && m.lr.flushes == 1);
  sim_run(&m, 120000);
  CHECK(m.result == 0);
  /* the LR1121 says "0": not known, the chunk goes without a check */
  sim_init(&m, 3, 1);
  m.lr.max_payload = 0;
  client_send(&m, 8, 1, false);
  CHECK(m.result == 1 && m.lr.flushes == 0);
  /* a chunk above what fits: INVALID, -EMSGSIZE as the size to chunk by is smaller than the chunk */
  sim_init(&m, 0, 1);
  m.lr.max_payload = 6;
  client_send(&m, 8, 1, false);
  CHECK(m.result == -EMSGSIZE && m.lr.frames == 0);
}

int main(void) {
  test_airtime();
  test_chunk_and_limits();
  test_duty_cycle_model();
  test_estimate();
  test_plan_send();
  test_txtrack();
  test_held();
  test_sim_normal();
  test_sim_frame_queued_past_watchdog();
  test_sim_stuck_frame();
  test_sim_busy_is_wait();
  test_sim_lost_response();
  test_sim_long_wait();
  test_sim_flush();
  /* DR0 to DR2: 51 B, DR3: 115 B, DR4 and up: 222 B */
  CHECK(lpwan_lora_max_payload(0, false) == 51);
  CHECK(lpwan_lora_max_payload(2, false) == 51);
  CHECK(lpwan_lora_max_payload(3, false) == 115);
  CHECK(lpwan_lora_max_payload(4, false) == 222);
  CHECK(lpwan_lora_max_payload(5, false) == 222);
  CHECK(lpwan_lora_max_payload(7, false) == 222);
  /* not an EU868 rate: the safe value */
  CHECK(lpwan_lora_max_payload(8, false) == 51);
  CHECK(lpwan_lora_max_payload(15, false) == 51);
  /* ADR: the network may lower the rate to DR0 whatever we configured */
  for (unsigned dr = 0; dr <= 15; dr++) {
    CHECK(lpwan_lora_max_payload((uint8_t)dr, true) == 51);
  }
  /* The node chunks by this value: a chunk of 3 B header and the payload must fit one uplink */
  CHECK(lpwan_lora_max_payload(5, false) <= 222);

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
