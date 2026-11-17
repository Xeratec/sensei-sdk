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
static int events_rx;
static sensei_LpwanEvent last_event;
static uint8_t last_downlink_len;

/* Faults, applied to the next transfers. */
static int corrupt_m2s;    /* flip a payload byte of the master frame */
static int corrupt_s2m;    /* flip a payload byte of the slave frame */
static int blank_s2m;      /* slave frame lost (master reads zeros) */
static int ready_timeouts; /* wait_ready times out */

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
  handler_calls = handler_sends = events_rx = 0;
  corrupt_m2s = corrupt_s2m = blank_s2m = ready_timeouts = 0;
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
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);
  /* radio mismatch */
  rq.send.radio = sensei_Radio_RADIO_NBIOT;
  CHECK(do_req(&rq, &rs) == 0 && rs.result != sensei_LpwanResult_LPWAN_OK);
  {
    sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

    ev.which_body = sensei_LpwanEvent_tx_done_tag;
    ev.tx_done.ok = true;
    lpwan_sm_event(&slave_sm, &ev);
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

int main(void) {
  test_roundtrip();
  test_state_rules();
  test_events();
  test_faults();
  test_event_faults();
  test_sm_table();
  printf("%s\n", check_failures ? "FAILED" : "ok");
  return check_failures ? 1 : 0;
}
