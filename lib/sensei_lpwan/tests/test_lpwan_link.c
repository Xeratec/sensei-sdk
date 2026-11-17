/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Master and slave cores of the LPWAN link against each other through a simulated SPI. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sensei_link/pb.h"
#include "sensei_lpwan/link_slave.h"
#include "sensei_lpwan/lpwan_master.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

/* ---- Simulated shield: slave core with a small request handler on the shared state machine ----
 */

static struct link_slave slv;
static struct lpwan_master mst;
static lpwan_sm_t slave_sm;
static lpwan_sm_t master_sm;

static const uint8_t *armed_tx;
static uint8_t *armed_rx;
static bool busy_pin = true;
static bool event_pin;
static uint32_t clock_ms;

static int handler_calls;
static int handler_sends;
static int handler_dups; /* a request executed twice */
static uint32_t handler_last_seq;
static int events_rx;
static uint32_t seen[16384]; /* seq of every tx_done event, in order of delivery */
static int nseen;
static sensei_LpwanEvent last_event;
static uint8_t last_downlink_len;

/* Faults, applied to the next transfers. */
static int corrupt_m2s;    /* flip a payload byte of the master frame */
static int corrupt_s2m;    /* flip a payload byte of the slave frame */
static int blank_s2m;      /* slave frame lost (master reads zeros) */
static int ready_timeouts; /* wait_ready times out */
/* Called with both frames of a transfer, after the copy: m2s is what the slave received, s2m what the
 * master received. May change them. */
static void (*fault_hook)(uint8_t *m2s, uint8_t *s2m);

static int s_arm(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  (void)c;
  (void)len;
  armed_tx = tx;
  armed_rx = rx;
  return 0;
}
static void s_busy(void *c, bool b) {
  (void)c;
  busy_pin = b;
}
static void s_event(void *c, bool e) {
  (void)c;
  event_pin = e;
}
static const struct link_slave_ops sops = {.arm = s_arm, .set_busy = s_busy, .set_event = s_event};

static void on_request(struct link_slave *s, const sensei_LpwanRequest *req, void *u) {
  sensei_LpwanResponse r = sensei_LpwanResponse_init_zero;

  (void)u;
  handler_calls++;
  handler_dups += req->seq == handler_last_seq;
  handler_last_seq = req->seq;
  r.seq = req->seq;
  r.result = lpwan_sm_admit(&slave_sm, req);
  if (r.result == sensei_LpwanResult_LPWAN_OK) {
    if (req->which_body == sensei_LpwanRequest_get_version_tag) {
      r.which_payload = sensei_LpwanResponse_version_tag;
      r.version.has_firmware = true;
      r.version.firmware.major = 1;
      strcpy(r.version.modem, "sim");
    } else if (req->which_body == sensei_LpwanRequest_send_tag) {
      handler_sends++;
    }
  }
  link_slave_respond(s, &r);
}

static int m_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  (void)c;
  if (!armed_tx) {
    return -EIO;
  }
  /* The slave must be ready: it armed buffers and released BUSY. */
  if (busy_pin) {
    return -EBUSY;
  }
  memcpy(armed_rx, tx, len);
  if (corrupt_m2s > 0 && tx[2] != LPWAN_FRAME_IDLE) {
    corrupt_m2s--;
    armed_rx[SENSEI_SPI_HEADER_SIZE] ^= 0x55;
  }
  memcpy(rx, armed_tx, len);
  if (blank_s2m > 0 && armed_tx[2] != LPWAN_FRAME_IDLE) {
    blank_s2m--;
    memset(rx, 0, len);
  } else if (corrupt_s2m > 0 && armed_tx[2] != LPWAN_FRAME_IDLE) {
    corrupt_s2m--;
    rx[SENSEI_SPI_HEADER_SIZE] ^= 0x55;
  }
  if (fault_hook) {
    fault_hook(armed_rx, rx);
  }
  busy_pin = true; /* ISR on the slave */
  armed_tx = NULL;
  clock_ms += 2;
  link_slave_done(&slv, 0);
  return 0;
}
static int m_ready(void *c, uint32_t t) {
  (void)c;
  if (ready_timeouts > 0) {
    ready_timeouts--;
    clock_ms += t;
    return -ETIMEDOUT;
  }
  return busy_pin ? -ETIMEDOUT : 0;
}
static bool m_event_pending(void *c) {
  (void)c;
  return event_pin;
}
static uint32_t m_now(void *c) {
  (void)c;
  return clock_ms;
}
static void m_on_event(void *c, const sensei_LpwanEvent *e) {
  (void)c;
  events_rx++;
  last_event = *e;
  if (e->which_body == sensei_LpwanEvent_tx_done_tag && nseen < (int)(sizeof(seen) / sizeof(seen[0]))) {
    seen[nseen++] = e->tx_done.seq;
  }
  if (e->which_body == sensei_LpwanEvent_downlink_rx_tag) {
    last_downlink_len = (uint8_t)(e->downlink_rx.payload.size & 0xFF);
  }
  lpwan_sm_event(&master_sm, e);
}
static const struct lpwan_master_ops mops = {
    .xfer = m_xfer, .wait_ready = m_ready, .event_pending = m_event_pending, .now_ms = m_now, .on_event = m_on_event};

static void reset_sim(void) {
  memset(&slv, 0, sizeof(slv));
  link_slave_init(&slv, &sops, NULL, on_request, NULL);
  lpwan_master_init(&mst, &mops, NULL);
  lpwan_sm_init(&slave_sm);
  lpwan_sm_init(&master_sm);
  handler_calls = handler_sends = handler_dups = events_rx = nseen = 0;
  handler_last_seq = 0;
  corrupt_m2s = corrupt_s2m = blank_s2m = ready_timeouts = 0;
  fault_hook = NULL;
  clock_ms = 0;
  armed_tx = NULL;
  link_slave_start(&slv);
}

static int do_req(sensei_LpwanRequest *rq, sensei_LpwanResponse *rs) { return lpwan_master_request(&mst, rq, rs, 100); }

static void test_roundtrip(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(rs.seq == rq.seq && rs.seq == 1);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(rs.which_payload == sensei_LpwanResponse_version_tag);
  CHECK(strcmp(rs.version.modem, "sim") == 0);
  CHECK(handler_calls == 1);
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == 2);
}

static void test_state_rules(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  uint32_t send_seq;

  reset_sim();
  /* send before connect */
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LORA;
  rq.send.payload.size = 4;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_NOT_JOINED);
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_NOT_CONNECTED);

  /* join LoRa, then LTE connect is refused (one radio at a time) */
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(slave_sm.state == LPWAN_ST_CONNECTING && slave_sm.tech == LPWAN_TECH_LORA);
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);
  /* configure while connecting is refused */
  rq.which_body = sensei_LpwanRequest_lora_configure_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);

  /* joined event, then send works and tx_done releases it */
  {
    sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

    ev.which_body = sensei_LpwanEvent_joined_tag;
    ev.joined.radio = sensei_Radio_RADIO_LORA;
    lpwan_sm_event(&slave_sm, &ev);
    CHECK(link_slave_push_event(&slv, &ev) == 0);
    CHECK(event_pin);
    CHECK(lpwan_master_fetch_events(&mst, 4) == 1);
    CHECK(!event_pin);
    CHECK(master_sm.state == LPWAN_ST_CONNECTED && master_sm.tech == LPWAN_TECH_LORA);
  }
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LORA;
  rq.send.payload.size = 4;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(slave_sm.state == LPWAN_ST_SENDING);
  send_seq = rq.seq;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);
  /* radio mismatch */
  rq.send.radio = sensei_Radio_RADIO_NBIOT;
  CHECK(do_req(&rq, &rs) == 0 && rs.result != sensei_LpwanResult_LPWAN_OK);
  {
    sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

    ev.which_body = sensei_LpwanEvent_tx_done_tag;
    ev.tx_done.ok = true;
    ev.tx_done.seq = send_seq + 1; /* another send: ignored (A7) */
    CHECK(!lpwan_sm_event(&slave_sm, &ev) && slave_sm.state == LPWAN_ST_SENDING);
    ev.tx_done.seq = send_seq;
    CHECK(lpwan_sm_event(&slave_sm, &ev));
    CHECK(slave_sm.state == LPWAN_ST_CONNECTED);
  }
  /* disconnect releases the technology */
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lte_disconnect_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(slave_sm.state == LPWAN_ST_DISCONNECTING);
}

static void test_events(void) {
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

  reset_sim();
  /* The event arrives while an idle frame is armed: one extra transfer is needed. */
  ev.which_body = sensei_LpwanEvent_network_time_tag;
  ev.network_time.unix_time = 1234;
  ev.network_time.radio = sensei_Radio_RADIO_LTEM;
  CHECK(link_slave_push_event(&slv, &ev) == 0);
  CHECK(event_pin);
  CHECK(lpwan_master_fetch_events(&mst, 8) == 1);
  CHECK(events_rx == 1);
  CHECK(last_event.which_body == sensei_LpwanEvent_network_time_tag);
  CHECK(last_event.network_time.unix_time == 1234);
  CHECK(!event_pin && link_slave_events_pending(&slv) == 0);

  /* A large downlink (1024 B) fits. */
  memset(&ev, 0, sizeof(ev));
  ev.which_body = sensei_LpwanEvent_downlink_rx_tag;
  ev.downlink_rx.radio = sensei_Radio_RADIO_LTEM;
  ev.downlink_rx.payload.size = 1024;
  for (int i = 0; i < 1024; i++) {
    ev.downlink_rx.payload.bytes[i] = (uint8_t)i;
  }
  events_rx = 0;
  CHECK(link_slave_push_event(&slv, &ev) == 0);
  CHECK(lpwan_master_fetch_events(&mst, 8) == 1);
  CHECK(last_event.which_body == sensei_LpwanEvent_downlink_rx_tag);
  CHECK(last_event.downlink_rx.payload.size == 1024);
  CHECK(last_event.downlink_rx.payload.bytes[1000] == (uint8_t)1000);

  /* Queue overflow is counted, the first events are kept in order. */
  events_rx = 0;
  for (int i = 0; i < 6; i++) {
    memset(&ev, 0, sizeof(ev));
    ev.which_body = sensei_LpwanEvent_tx_done_tag;
    ev.tx_done.seq = (uint32_t)i;
    (void)link_slave_push_event(&slv, &ev);
  }
  CHECK(slv.n_dropped_events == 2);
  CHECK(lpwan_master_fetch_events(&mst, 16) == 4);
  CHECK(last_event.tx_done.seq == 3);

  /* An event is also delivered inside request(). */
  {
    sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
    sensei_LpwanResponse rs;

    events_rx = 0;
    memset(&ev, 0, sizeof(ev));
    ev.which_body = sensei_LpwanEvent_connected_tag;
    ev.connected.radio = sensei_Radio_RADIO_NBIOT;
    link_slave_push_event(&slv, &ev);
    rq.which_body = sensei_LpwanRequest_get_status_tag;
    CHECK(do_req(&rq, &rs) == 0);
    (void)lpwan_master_fetch_events(&mst, 4);
    CHECK(events_rx == 1);
  }
}

static void test_faults(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  /* Corrupt request: slave flags it, master repeats it, handler runs once. */
  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  corrupt_m2s = 1;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(rs.seq == rq.seq && handler_calls == 1);
  CHECK(slv.n_bad_frames >= 1);

  /* Corrupt response: master flags it, slave sends it again, handler still runs once. */
  reset_sim();
  corrupt_s2m = 1;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(handler_calls == 1);
  CHECK(mst.n_bad_frames >= 1 && slv.n_repeats >= 1);

  /* Lost response (blank): same. */
  reset_sim();
  blank_s2m = 1;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(handler_calls == 1);

  /* Many faults in a row still do not execute a request twice. */
  reset_sim();
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  slave_sm.state = LPWAN_ST_CONNECTED;
  slave_sm.tech = LPWAN_TECH_LTE;
  corrupt_s2m = 3;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(handler_sends == 1);

  /* Slave never ready: timeout after the retries, nothing executed. */
  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  ready_timeouts = 100;
  CHECK(do_req(&rq, &rs) == -ETIMEDOUT);
  CHECK(handler_calls == 0 && mst.n_retries == 2);

  /* Slave becomes ready after one timeout: retry works. */
  reset_sim();
  ready_timeouts = 1;
  CHECK(do_req(&rq, &rs) == 0 && handler_calls == 1);

  /* Sequence numbers keep counting and a stale response is not taken for the new one. */
  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  for (int i = 1; i <= 5; i++) {
    CHECK(do_req(&rq, &rs) == 0 && rs.seq == (uint32_t)i);
  }
}

static void test_event_faults(void) {
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  /* A damaged event is sent again and delivered exactly once, in order. */
  reset_sim();
  ev.which_body = sensei_LpwanEvent_tx_done_tag;
  ev.tx_done.seq = 1;
  link_slave_push_event(&slv, &ev);
  ev.tx_done.seq = 2;
  link_slave_push_event(&slv, &ev);
  corrupt_s2m = 2;
  CHECK(lpwan_master_fetch_events(&mst, 16) == 2);
  CHECK(events_rx == 2 && last_event.tx_done.seq == 2);
  CHECK(link_slave_events_pending(&slv) == 0 && !event_pin);

  /* The response of an old request that was never confirmed does not hide new events or
   * responses, and is not delivered twice. */
  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == 1);
  ev.tx_done.seq = 7;
  link_slave_push_event(&slv, &ev);
  CHECK(lpwan_master_fetch_events(&mst, 16) == 1);
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == 2);
  CHECK(handler_calls == 2);

  /* Many requests with events between them. */
  for (int i = 0; i < 20; i++) {
    ev.tx_done.seq = (uint32_t)(100 + i);
    link_slave_push_event(&slv, &ev);
    CHECK(do_req(&rq, &rs) == 0);
    (void)lpwan_master_fetch_events(&mst, 16);
  }
  CHECK(last_event.tx_done.seq == 119 && handler_calls == 22);
}

static void test_sm_table(void) {
  lpwan_sm_t sm;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_lte_configure_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  CHECK(sm.state == LPWAN_ST_CONNECTING && sm.tech == LPWAN_TECH_LTE);
  rq.which_body = sensei_LpwanRequest_get_time_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_NOT_CONNECTED);
  rq.which_body = sensei_LpwanRequest_sleep_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_BUSY);

  /* connect failure returns to idle and releases the radio */
  {
    sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

    ev.which_body = sensei_LpwanEvent_error_tag;
    ev.error.code = sensei_LpwanResult_LPWAN_NO_NETWORK;
    lpwan_sm_event(&sm, &ev);
    CHECK(sm.state == LPWAN_ST_IDLE && sm.tech == LPWAN_TECH_NONE);
  }
  /* BLE claim excludes LPWAN radios */
  CHECK(lpwan_sm_claim_ble(&sm, 1) == sensei_LpwanResult_LPWAN_OK);
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(lpwan_sm_claim_ble(&sm, 0) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_sm_claim_ble(&sm, 1) == sensei_LpwanResult_LPWAN_BUSY);

  /* error state accepts only queries, disconnect and no new connection */
  sm.state = LPWAN_ST_ERROR;
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_FAILED);
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  rq.which_body = sensei_LpwanRequest_lte_disconnect_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);

  /* the connection evaluation needs a connected LTE modem and does not move the machine */
  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_lte_eval_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_NOT_CONNECTED);
  sm.state = LPWAN_ST_CONNECTED;
  sm.tech = LPWAN_TECH_LORA;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_NOT_CONNECTED);
  sm.tech = LPWAN_TECH_LTE;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK && sm.state == LPWAN_ST_CONNECTED);
  sm.state = LPWAN_ST_SENDING;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_BUSY);

  /* the update needs the radios idle; its status and the confirm are always answered */
  lpwan_sm_init(&sm);
  for (pb_size_t t = sensei_LpwanRequest_dfu_begin_tag; t <= sensei_LpwanRequest_dfu_abort_tag; t++) {
    rq.which_body = t;
    CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK && sm.state == LPWAN_ST_IDLE);
  }
  sm.state = LPWAN_ST_CONNECTED;
  sm.tech = LPWAN_TECH_LTE;
  for (pb_size_t t = sensei_LpwanRequest_dfu_begin_tag; t <= sensei_LpwanRequest_dfu_abort_tag; t++) {
    rq.which_body = t;
    CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_BUSY);
  }
  rq.which_body = sensei_LpwanRequest_dfu_status_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  rq.which_body = sensei_LpwanRequest_dfu_confirm_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);

  /* a restored LoRa session (joined event in idle) connects the machine */
  lpwan_sm_init(&sm);
  {
    sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

    ev.which_body = sensei_LpwanEvent_joined_tag;
    ev.joined.radio = sensei_Radio_RADIO_LORA;
    lpwan_sm_event(&sm, &ev);
    CHECK(sm.state == LPWAN_ST_CONNECTED && sm.tech == LPWAN_TECH_LORA);
  }
}

/* ---- helpers for the fault tests ---- */

static void push_tx_done(uint32_t seq) {
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

  ev.which_body = sensei_LpwanEvent_tx_done_tag;
  ev.tx_done.seq = seq;
  ev.tx_done.ok = true;
  CHECK(link_slave_push_event(&slv, &ev) == 0);
}

/* Slave restart without a master restart (shield power cycle): what the master keeps is its own. */
static void restart_slave(void) {
  memset(&slv, 0, sizeof(slv));
  link_slave_init(&slv, &sops, NULL, on_request, NULL);
  lpwan_sm_init(&slave_sm);
  armed_tx = NULL;
  busy_pin = true;
  link_slave_start(&slv);
}

static int msg_frame_no, corrupt_msg_frame;
static void hook_corrupt_nth(uint8_t *m2s, uint8_t *s2m) {
  (void)m2s;
  if (s2m[2] != LPWAN_FRAME_IDLE) {
    if (msg_frame_no == corrupt_msg_frame) {
      s2m[SENSEI_SPI_HEADER_SIZE] ^= 0x55;
    }
    msg_frame_no++;
  }
}

/* B2: the ACK names the message it confirms. A damaged first transmission of a message is not
 * released by the ACK of the one before; every event arrives once, in order. */
static void test_ack_names_message(void) {
  for (int at = 0; at < 8; at++) {
    reset_sim();
    fault_hook = hook_corrupt_nth;
    msg_frame_no = 0;
    corrupt_msg_frame = at;
    for (uint32_t i = 1; i <= 3; i++) {
      push_tx_done(i);
    }
    CHECK(lpwan_master_fetch_events(&mst, 32) == 3);
    CHECK(nseen == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3);
    CHECK(link_slave_events_pending(&slv) == 0 && !event_pin);
  }
}

static void master_frame(uint8_t flags) {
  memset(armed_rx, 0, LPWAN_XFER_LEN);
  sensei_spi_header_pack(armed_rx, LPWAN_FRAME_IDLE, flags, armed_rx + SENSEI_SPI_HEADER_SIZE, 0);
  link_slave_done(&slv, 0);
}

/* B2 at the slave: an ACK for another toggle bit does not release the armed message. */
static void test_stale_ack(void) {
  reset_sim();
  push_tx_done(1);
  master_frame(0); /* the event is armed now, toggle bit 1 */
  CHECK(slv.armed != 0 && link_slave_events_pending(&slv) == 1);
  master_frame(LPWAN_FLAG_ACK); /* ACK of toggle 0: the one before */
  CHECK(slv.armed != 0 && link_slave_events_pending(&slv) == 1);
  master_frame(LPWAN_FLAG_ACK_TOGGLE); /* no ACK flag */
  CHECK(slv.armed != 0 && link_slave_events_pending(&slv) == 1);
  master_frame(LPWAN_FLAG_ACK | LPWAN_FLAG_ACK_TOGGLE);
  CHECK(slv.armed == 0 && link_slave_events_pending(&slv) == 0);
  /* a frame whose flags were damaged is dropped by the CRC, even if it would have been an ACK */
  push_tx_done(2);
  master_frame(0);
  CHECK(slv.armed != 0);
  memset(armed_rx, 0, LPWAN_XFER_LEN);
  sensei_spi_header_pack(armed_rx, LPWAN_FRAME_IDLE, 0, armed_rx + SENSEI_SPI_HEADER_SIZE, 0);
  armed_rx[3] = LPWAN_FLAG_ACK | LPWAN_FLAG_TOGGLE; /* the toggle of the armed message is 0 */
  armed_rx[3] = LPWAN_FLAG_ACK | LPWAN_FLAG_ACK_TOGGLE;
  link_slave_done(&slv, 0);
  CHECK(slv.armed != 0 && link_slave_events_pending(&slv) == 1 && slv.n_bad_frames == 1);
}

static uint32_t rng_state;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}
static unsigned fault_pct;
static void hook_random(uint8_t *m2s, uint8_t *s2m) {
  uint8_t *f[2] = {m2s, s2m};

  for (int d = 0; d < 2; d++) {
    if (rnd() % 100 < fault_pct) {
      f[d][rnd() % 24] ^= (uint8_t)(1u << (rnd() % 8)); /* header and the start of the payload */
    }
  }
  if (rnd() % 100 < fault_pct / 3) {
    memset(s2m, 0, SENSEI_SPI_HEADER_SIZE); /* slave frame lost */
  }
}

/* Events and requests under random corruption of both directions: every event once and in order,
 * no request executed twice. */
static void random_faults(uint32_t seeds, int rounds, unsigned pct) {
  for (uint32_t seed = 1; seed <= seeds; seed++) {
    uint32_t next = 1;
    sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
    sensei_LpwanResponse rs;

    reset_sim();
    rng_state = seed * 2654435761u;
    fault_pct = pct;
    fault_hook = hook_random;
    mst.retries = 6;
    rq.which_body = sensei_LpwanRequest_get_status_tag;
    for (int round = 0; round < rounds; round++) {
      unsigned k = rnd() % 4;

      while (k-- > 0 && link_slave_events_pending(&slv) < LINK_SLAVE_EVENT_SLOTS) {
        push_tx_done(next++);
      }
      if (rnd() % 3 == 0) {
        int err = do_req(&rq, &rs);

        /* Two damaged version bytes in a row read as another frame version (D9): the master does not latch, the
         * next request goes through, the client asks again after a pause. */
        CHECK(err == 0 ? rs.seq == rq.seq : (err == -ETIMEDOUT || err == -EPROTONOSUPPORT));
      }
      (void)lpwan_master_fetch_events(&mst, 32);
    }
    fault_hook = NULL; /* faults off: everything still queued arrives */
    for (int i = 0; i < 8 && (link_slave_events_pending(&slv) > 0 || event_pin); i++) {
      (void)lpwan_master_fetch_events(&mst, 32);
    }
    CHECK(link_slave_events_pending(&slv) == 0);
    CHECK(nseen == (int)next - 1);
    for (int i = 0; i < nseen; i++) {
      CHECK(seen[i] == (uint32_t)i + 1);
    }
    CHECK(handler_dups == 0);
    CHECK(do_req(&rq, &rs) == 0 && mst.failures == 0);
  }
}

static void test_random_faults(void) {
  random_faults(40, 60, 12);
  /* Long runs at 10 % and 30 % damage (the review fuzz): thousands of events, none lost, repeated or reordered. */
  random_faults(4, 3000, 10);
  random_faults(4, 3000, 30);
}

/* A28, B16: the shield restarted. With lpwan_master_reset() the first message of the new session
 * is taken at once. */
static void test_shield_restart(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  uint32_t t0;

  rq.which_body = sensei_LpwanRequest_get_version_tag;
  reset_sim();
  CHECK(do_req(&rq, &rs) == 0);
  restart_slave();
  lpwan_master_reset(&mst);
  handler_calls = 0;
  t0 = clock_ms;
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == rq.seq);
  CHECK(handler_calls == 1 && mst.n_retries == 0 && clock_ms - t0 < 20);

  /* The first event of the new session is delivered too (the master took events before the restart). */
  reset_sim();
  push_tx_done(1);
  CHECK(lpwan_master_fetch_events(&mst, 8) == 1);
  restart_slave();
  lpwan_master_reset(&mst);
  push_tx_done(2);
  CHECK(do_req(&rq, &rs) == 0);
  (void)lpwan_master_fetch_events(&mst, 8);
  CHECK(nseen == 2 && seen[1] == 2);

  /* Without the reset the first response of the new session has the toggle bit the master saw last:
   * it is taken as a repeat. The request still gets through with a retry. */
  reset_sim();
  CHECK(do_req(&rq, &rs) == 0);
  restart_slave();
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == rq.seq);

  /* The master restarted (lpwan_master_init) while the slave kept its toggle bit: the first
   * message is taken whatever its bit is. */
  for (int before = 0; before < 4; before++) {
    reset_sim();
    for (int i = 0; i < before; i++) {
      push_tx_done((uint32_t)i + 1);
    }
    CHECK(lpwan_master_fetch_events(&mst, 16) == before);
    push_tx_done(100);
    lpwan_master_init(&mst, &mops, NULL);
    nseen = 0;
    CHECK(lpwan_master_fetch_events(&mst, 16) == 1);
    CHECK(nseen == 1 && seen[0] == 100 && link_slave_events_pending(&slv) == 0);
  }
}

/* B18: the ACK of a response waits for a frame that is sent; a response that does not decode is
 * not confirmed. */
static int garbage_left;
static void hook_garbage_response(uint8_t *m2s, uint8_t *s2m) {
  (void)m2s;
  if (garbage_left > 0 && s2m[2] == SENSEI_SPI_LPWAN_RESPONSE) {
    uint8_t flags = s2m[3];

    garbage_left--;
    memset(s2m + SENSEI_SPI_HEADER_SIZE, 0xFF, 2); /* not a protobuf message, valid frame */
    sensei_spi_header_pack(s2m, SENSEI_SPI_LPWAN_RESPONSE, flags, s2m + SENSEI_SPI_HEADER_SIZE, 2);
  }
}

static void test_ack_after_response(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(do_req(&rq, &rs) == 0);
  CHECK(mst.ack_pending);      /* no frame carried it yet */
  CHECK(slv.response_pending); /* so the slave still holds the response */
  push_tx_done(1);
  CHECK(lpwan_master_fetch_events(&mst, 8) == 1); /* this exchange sends the ACK and brings the event */
  CHECK(!slv.response_pending && link_slave_events_pending(&slv) == 0);

  reset_sim();
  fault_hook = hook_garbage_response;
  garbage_left = 1;
  CHECK(do_req(&rq, &rs) == 0 && rs.seq == rq.seq);
  CHECK(handler_calls == 1 && garbage_left == 0 && slv.n_repeats >= 1); /* sent again, not acked first */
}

/* B24 (link level): another SPI frame version is reported as -EPROTONOSUPPORT. A9: the master
 * counts the requests that fail in a row. */
static void hook_wrong_version(uint8_t *m2s, uint8_t *s2m) {
  (void)m2s;
  if (s2m[0] == SENSEI_SPI_MAGIC) {
    s2m[1] = SENSEI_LINK_VERSION + 1;
  }
}

static void test_failures_and_version(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  reset_sim();
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(mst.failures == 0);
  ready_timeouts = 100;
  CHECK(do_req(&rq, &rs) == -ETIMEDOUT && mst.failures == 1);
  CHECK(do_req(&rq, &rs) == -ETIMEDOUT && mst.failures == 2);
  ready_timeouts = 0;
  CHECK(do_req(&rq, &rs) == 0 && mst.failures == 0);

  fault_hook = hook_wrong_version;
  CHECK(do_req(&rq, &rs) == -EPROTONOSUPPORT && mst.failures == 1);
  fault_hook = NULL;
  CHECK(do_req(&rq, &rs) == 0 && mst.failures == 0);
}

/* B11: downlinks and time cannot take the slots that state events need. */
static void test_event_reserve(void) {
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

  reset_sim();
  ev.which_body = sensei_LpwanEvent_network_time_tag;
  for (int i = 0; i < 6; i++) {
    (void)link_slave_push_event(&slv, &ev);
  }
  CHECK(link_slave_events_pending(&slv) == LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED);
  CHECK(link_slave_dropped_events(&slv) == 6 - (LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED));
  push_tx_done(1);
  push_tx_done(2); /* both fit in the reserved slots */
  CHECK(link_slave_events_pending(&slv) == LINK_SLAVE_EVENT_SLOTS);
  ev.which_body = sensei_LpwanEvent_error_tag;
  CHECK(link_slave_push_event(&slv, &ev) == -ENOMEM); /* really full */
}

/* ---- state machine: events of the wrong radio, stale tx_done, undo (B10, A3, A7, B19) ---- */

static void test_sm_events(void) {
  lpwan_sm_t sm, before, admitted;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

  /* A late LoRa join does not complete an LTE connect. */
  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  ev.which_body = sensei_LpwanEvent_joined_tag;
  ev.joined.radio = sensei_Radio_RADIO_LORA;
  CHECK(!lpwan_sm_event(&sm, &ev));
  CHECK(sm.state == LPWAN_ST_CONNECTING && sm.tech == LPWAN_TECH_LTE);
  ev.which_body = sensei_LpwanEvent_connected_tag; /* "connected" is LTE: a LoRa radio id is wrong */
  CHECK(!lpwan_sm_event(&sm, &ev));
  ev.connected.radio = sensei_Radio_RADIO_NBIOT;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED && sm.tech == LPWAN_TECH_LTE);
  /* connected twice: the second one changes nothing */
  CHECK(!lpwan_sm_event(&sm, &ev));

  /* A LoRa "disconnected" does not end a BLE claim or an LTE link. */
  CHECK(lpwan_sm_claim_ble(&sm, 1) == sensei_LpwanResult_LPWAN_BUSY);
  ev.which_body = sensei_LpwanEvent_disconnected_tag;
  ev.disconnected.radio = sensei_Radio_RADIO_LORA;
  CHECK(!lpwan_sm_event(&sm, &ev) && sm.tech == LPWAN_TECH_LTE && sm.state == LPWAN_ST_CONNECTED);
  ev.disconnected.radio = sensei_Radio_RADIO_LTEM;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_IDLE && sm.tech == LPWAN_TECH_NONE);
  CHECK(lpwan_sm_claim_ble(&sm, 1) == sensei_LpwanResult_LPWAN_OK);
  ev.disconnected.radio = sensei_Radio_RADIO_LORA;
  CHECK(!lpwan_sm_event(&sm, &ev) && sm.tech == LPWAN_TECH_BLE);

  /* A3: a send that timed out is given up; the next send is admitted, the late tx_done of the first is
   * ignored, the one of the second ends it (A7). */
  lpwan_sm_init(&sm);
  sm.state = LPWAN_ST_CONNECTED;
  sm.tech = LPWAN_TECH_LTE;
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  rq.seq = 7;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK && sm.state == LPWAN_ST_SENDING);
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_BUSY); /* the bug: stays so for ever */
  lpwan_sm_abort_send(&sm);
  CHECK(sm.state == LPWAN_ST_CONNECTED);
  lpwan_sm_abort_send(&sm); /* not sending: no change */
  CHECK(sm.state == LPWAN_ST_CONNECTED);
  rq.seq = 8;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK && sm.send_seq == 8);
  memset(&ev, 0, sizeof(ev));
  ev.which_body = sensei_LpwanEvent_tx_done_tag;
  ev.tx_done.seq = 7;
  CHECK(!lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_SENDING);
  ev.tx_done.seq = 8;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED);

  /* B19: undo the admission only if nothing moved the machine meanwhile. */
  lpwan_sm_init(&before);
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  sm = before;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  admitted = sm;
  lpwan_sm_revert(&sm, &before, &admitted); /* the request failed, no event: back to idle */
  CHECK(sm.state == LPWAN_ST_IDLE && sm.tech == LPWAN_TECH_NONE);
  sm = admitted;
  memset(&ev, 0, sizeof(ev));
  ev.which_body = sensei_LpwanEvent_connected_tag; /* connected arrived during the request */
  ev.connected.radio = sensei_Radio_RADIO_LTEM;
  CHECK(lpwan_sm_event(&sm, &ev));
  lpwan_sm_revert(&sm, &before, &admitted); /* the request then failed: keep what the event did */
  CHECK(sm.state == LPWAN_ST_CONNECTED && sm.tech == LPWAN_TECH_LTE);

  /* which events move the machine */
  memset(&ev, 0, sizeof(ev));
  ev.which_body = sensei_LpwanEvent_downlink_rx_tag;
  CHECK(!lpwan_event_is_state(&ev));
  ev.which_body = sensei_LpwanEvent_network_time_tag;
  CHECK(!lpwan_event_is_state(&ev));
  ev.which_body = sensei_LpwanEvent_tx_done_tag;
  CHECK(lpwan_event_is_state(&ev));
}

/* D10: every send has one result. A second tx_done for the same seq (the watchdog first, the real one late) and a
 * tx_done of an older send never end a later send, also not after a request that failed and was undone. An error
 * event does not end a send. The seq is known to the machine before the request goes out (simplification). */
static void test_sm_send_results(void) {
  lpwan_sm_t sm, before, admitted;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;
  struct lpwan_master m;

  lpwan_master_init(&m, &mops, NULL);
  CHECK(lpwan_master_next_seq(&m) == 1);
  m.seq = 41;
  CHECK(lpwan_master_next_seq(&m) == 42);
  m.seq = UINT32_MAX;
  CHECK(lpwan_master_next_seq(&m) == 1); /* 0 is never a seq: it means "none" */

  lpwan_sm_init(&sm);
  sm.state = LPWAN_ST_CONNECTED;
  sm.tech = LPWAN_TECH_LORA;
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LORA;
  ev.which_body = sensei_LpwanEvent_tx_done_tag;

  /* The result can come before the response of the request is read: the seq is in the machine from the start. */
  rq.seq = 5;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK && sm.send_seq == 5);
  ev.tx_done.seq = 5;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED && sm.send_seq == 0);
  CHECK(!lpwan_sm_event(&sm, &ev)); /* second result of the same send */

  /* An error event while sending does not end the send: only its tx_done does. */
  rq.seq = 6;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  {
    sensei_LpwanEvent err = sensei_LpwanEvent_init_zero;

    err.which_body = sensei_LpwanEvent_error_tag;
    err.error.code = sensei_LpwanResult_LPWAN_NO_NETWORK;
    CHECK(lpwan_sm_event(&sm, &err));
    err.error.code = sensei_LpwanResult_LPWAN_FAILED;
    CHECK(lpwan_sm_event(&sm, &err));
    CHECK(sm.state == LPWAN_ST_SENDING && sm.send_seq == 6);
  }
  ev.tx_done.seq = 6;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED);

  /* The watchdog result (failed) came first, the next send was refused and undone, then the real result of the
   * first send arrives late: it must not touch the machine, and not the send after it either. */
  rq.seq = 7;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  ev.tx_done.seq = 7;
  ev.tx_done.ok = false;
  CHECK(lpwan_sm_event(&sm, &ev));
  before = sm;
  rq.seq = 8;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  admitted = sm;
  lpwan_sm_revert(&sm, &before, &admitted); /* the request failed */
  CHECK(sm.state == LPWAN_ST_CONNECTED && sm.send_seq == 0);
  ev.tx_done.seq = 7; /* late */
  CHECK(!lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED);
  rq.seq = 9;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  ev.tx_done.seq = 7;
  CHECK(!lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_SENDING); /* does not end send 9 */
  ev.tx_done.seq = 9;
  CHECK(lpwan_sm_event(&sm, &ev) && sm.state == LPWAN_ST_CONNECTED);

  /* A send given up by timeout leaves no seq behind. */
  rq.seq = 10;
  CHECK(lpwan_sm_admit(&sm, &rq) == sensei_LpwanResult_LPWAN_OK);
  lpwan_sm_abort_send(&sm);
  CHECK(sm.state == LPWAN_ST_CONNECTED && sm.send_seq == 0);
  ev.tx_done.seq = 10;
  CHECK(!lpwan_sm_event(&sm, &ev));
}

int main(void) {
  test_roundtrip();
  test_state_rules();
  test_events();
  test_faults();
  test_event_faults();
  test_sm_table();
  test_ack_names_message();
  test_stale_ack();
  test_random_faults();
  test_shield_restart();
  test_ack_after_response();
  test_failures_and_version();
  test_event_reserve();
  test_sm_events();
  test_sm_send_results();
  printf("%s\n", check_failures ? "FAILED" : "ok");
  return check_failures ? 1 : 0;
}
