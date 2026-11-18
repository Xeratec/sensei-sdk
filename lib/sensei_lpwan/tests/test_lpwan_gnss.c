/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* GNSS of the shield: the rules (lpwan_gnss.h) and the new link messages (gnss_fix, gnss_cancel, gnss_result,
 * LpwanVersion.gnss, the GNSS counters of LpwanStatus) between the master and the slave core through a simulated SPI.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensei_lpwan/link_slave.h"
#include "sensei_lpwan/lpwan_gnss.h"
#include "sensei_lpwan/lpwan_master.h"

static int check_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                                  \
      check_failures++;                                                                                                \
    }                                                                                                                  \
  } while (0)

/* ---- the rules ---- */

static void test_timeout(void) {
  uint32_t t = 0;

  CHECK(lpwan_gnss_timeout(0, &t) && t == LPWAN_GNSS_TIMEOUT_DEFAULT_S);
  CHECK(lpwan_gnss_timeout(1, &t) && t == 1);
  CHECK(lpwan_gnss_timeout(900, &t) && t == 900);
  CHECK(!lpwan_gnss_timeout(901, &t));
  CHECK(lpwan_gnss_wait_ms(120) == (120 + LPWAN_GNSS_MARGIN_S) * 1000u);
}

static void test_admit(void) {
  static const lpwan_state_t states[] = {LPWAN_ST_IDLE,    LPWAN_ST_CONNECTING,    LPWAN_ST_CONNECTED,
                                         LPWAN_ST_SENDING, LPWAN_ST_DISCONNECTING, LPWAN_ST_ERROR};
  static const lpwan_tech_t techs[] = {LPWAN_TECH_NONE, LPWAN_TECH_LTE, LPWAN_TECH_LORA, LPWAN_TECH_BLE};

  for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
    for (unsigned j = 0; j < sizeof(techs) / sizeof(techs[0]); j++) {
      lpwan_sm_t sm = {.state = states[i], .tech = techs[j]};
      bool lte_busy =
          techs[j] == LPWAN_TECH_LTE &&
          (states[i] == LPWAN_ST_CONNECTING || states[i] == LPWAN_ST_SENDING || states[i] == LPWAN_ST_DISCONNECTING);

      CHECK(lpwan_gnss_admit(&sm, false) == (lte_busy ? sensei_LpwanResult_LPWAN_BUSY : sensei_LpwanResult_LPWAN_OK));
      CHECK(lpwan_gnss_admit(&sm, true) == sensei_LpwanResult_LPWAN_BUSY); /* one fix at a time */
    }
  }
  /* LoRa is not touched by a fix, whatever it does; LTE registered and idle is fine (the modem time-shares) */
  CHECK(lpwan_gnss_admit(&(lpwan_sm_t){.state = LPWAN_ST_SENDING, .tech = LPWAN_TECH_LORA}, false) ==
        sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_gnss_admit(&(lpwan_sm_t){.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LTE}, false) ==
        sensei_LpwanResult_LPWAN_OK);
}

static void test_refuses(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  lpwan_sm_t lte = {.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LTE};
  lpwan_sm_t lora = {.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LORA};
  lpwan_sm_t none = {.state = LPWAN_ST_IDLE, .tech = LPWAN_TECH_NONE};

  /* what changes the functional mode of the modem waits for the fix */
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(lpwan_gnss_refuses(&rq, &none));
  rq.which_body = sensei_LpwanRequest_lte_configure_tag;
  CHECK(lpwan_gnss_refuses(&rq, &none));
  rq.which_body = sensei_LpwanRequest_sleep_tag;
  CHECK(lpwan_gnss_refuses(&rq, &none));
  CHECK(lpwan_gnss_refuses(&rq, &lora)); /* a sleep of any radio */
  rq.which_body = sensei_LpwanRequest_lte_disconnect_tag;
  CHECK(lpwan_gnss_refuses(&rq, &lte));
  CHECK(!lpwan_gnss_refuses(&rq, &lora)); /* a LoRa leave does not touch the modem */
  CHECK(!lpwan_gnss_refuses(&rq, &none));
  rq.which_body = sensei_LpwanRequest_dfu_begin_tag; /* the apply restarts the modem library */
  CHECK(lpwan_gnss_refuses(&rq, &none));
  /* LTE sends and the evaluation go on: the modem gives LTE priority */
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  CHECK(!lpwan_gnss_refuses(&rq, &lte));
  rq.send.radio = sensei_Radio_RADIO_NBIOT;
  CHECK(!lpwan_gnss_refuses(&rq, &lte));
  rq.send.radio = sensei_Radio_RADIO_LORA;
  CHECK(!lpwan_gnss_refuses(&rq, &lora));
  rq.which_body = sensei_LpwanRequest_lte_eval_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &lte));
  /* the rest goes on */
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &lte));
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &none));
  rq.which_body = sensei_LpwanRequest_dfu_chunk_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &none));
  rq.which_body = sensei_LpwanRequest_gnss_cancel_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &lte));
  rq.which_body = sensei_LpwanRequest_gnss_fix_tag;
  CHECK(!lpwan_gnss_refuses(&rq, &lte)); /* the admission says BUSY itself */
}

static void test_allowed_during_dfu(void) {
  static const pb_size_t ok[] = {sensei_LpwanRequest_get_version_tag, sensei_LpwanRequest_get_status_tag,
                                 sensei_LpwanRequest_diag_ack_tag,    sensei_LpwanRequest_lte_disconnect_tag,
                                 sensei_LpwanRequest_gnss_cancel_tag, sensei_LpwanRequest_dfu_begin_tag,
                                 sensei_LpwanRequest_dfu_chunk_tag,   sensei_LpwanRequest_dfu_confirm_tag};
  static const pb_size_t no[] = {sensei_LpwanRequest_gnss_fix_tag, sensei_LpwanRequest_lte_connect_tag,
                                 sensei_LpwanRequest_send_tag, sensei_LpwanRequest_sleep_tag,
                                 sensei_LpwanRequest_lte_configure_tag};
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
    rq.which_body = ok[i];
    CHECK(lpwan_allowed_during_dfu(&rq));
  }
  for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
    rq.which_body = no[i];
    CHECK(!lpwan_allowed_during_dfu(&rq));
  }
}

/* The order of the checks of the shield (service.c calls lpwan_admit_request()): an update, then the fix rules, then
 * the state machine. The state machine does not move on a refusal. */
static void test_admit_request(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  lpwan_sm_t sm;

  /* a fix request: refused while an update holds the link, while a fix runs, while LTE is busy */
  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_gnss_fix_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, false) == sensei_LpwanResult_LPWAN_OK);
  CHECK(lpwan_admit_request(&sm, &rq, true, false) == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  sm = (lpwan_sm_t){.state = LPWAN_ST_SENDING, .tech = LPWAN_TECH_LTE};
  CHECK(lpwan_admit_request(&sm, &rq, false, false) == sensei_LpwanResult_LPWAN_BUSY);
  sm.tech = LPWAN_TECH_LORA;
  CHECK(lpwan_admit_request(&sm, &rq, false, false) == sensei_LpwanResult_LPWAN_OK);

  /* the update: no fix meanwhile, the cancel goes through, and no update starts under a fix */
  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_gnss_cancel_tag;
  CHECK(lpwan_admit_request(&sm, &rq, true, true) == sensei_LpwanResult_LPWAN_OK);
  rq.which_body = sensei_LpwanRequest_dfu_begin_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(lpwan_admit_request(&sm, &rq, false, false) == sensei_LpwanResult_LPWAN_OK);

  /* a fix runs: LTE connect, configure and sleep are refused and the state machine stays put */
  lpwan_sm_init(&sm);
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(sm.state == LPWAN_ST_IDLE && sm.tech == LPWAN_TECH_NONE);
  rq.which_body = sensei_LpwanRequest_lte_configure_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  rq.which_body = sensei_LpwanRequest_sleep_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  /* the same requests without a fix go through the state machine */
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, false) == sensei_LpwanResult_LPWAN_OK);
  CHECK(sm.state == LPWAN_ST_CONNECTING && sm.tech == LPWAN_TECH_LTE);

  /* LTE connected, a fix runs: the send goes through (the state machine takes it), the release is refused */
  sm = (lpwan_sm_t){.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LTE};
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  rq.send.payload.size = 4;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_OK);
  CHECK(sm.state == LPWAN_ST_SENDING);
  sm = (lpwan_sm_t){.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LTE};
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lte_disconnect_tag;
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(sm.state == LPWAN_ST_CONNECTED);
  CHECK(lpwan_admit_request(&sm, &rq, true, false) == sensei_LpwanResult_LPWAN_OK); /* allowed during an update */
  CHECK(sm.state == LPWAN_ST_DISCONNECTING);
  /* LoRa leave and LoRa sends are not touched by a fix */
  sm = (lpwan_sm_t){.state = LPWAN_ST_CONNECTED, .tech = LPWAN_TECH_LORA};
  CHECK(lpwan_admit_request(&sm, &rq, false, true) == sensei_LpwanResult_LPWAN_OK);
}

static void test_end_status(void) {
  CHECK(lpwan_gnss_end_status(true, false, false, false) == sensei_GnssFixStatus_GNSS_FIX_OK);
  CHECK(lpwan_gnss_end_status(true, true, true, true) == sensei_GnssFixStatus_GNSS_FIX_OK); /* a fix wins */
  CHECK(lpwan_gnss_end_status(false, true, false, true) == sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  CHECK(lpwan_gnss_end_status(false, false, true, true) == sensei_GnssFixStatus_GNSS_FIX_FAILED);
  CHECK(lpwan_gnss_end_status(false, false, false, true) == sensei_GnssFixStatus_GNSS_FIX_BLOCKED);
  CHECK(lpwan_gnss_end_status(false, false, false, false) == sensei_GnssFixStatus_GNSS_FIX_TIMEOUT);
  CHECK(strcmp(lpwan_gnss_status_name(sensei_GnssFixStatus_GNSS_FIX_BLOCKED), "blocked") == 0);
  CHECK(strcmp(lpwan_gnss_status_name((sensei_GnssFixStatus)99), "?") == 0);
}

static void test_utc(void) {
  CHECK(lpwan_gnss_utc_to_unix(2026, 6, 1, 0, 0, 0) == 1780272000u);
  CHECK(lpwan_gnss_utc_to_unix(2020, 1, 1, 0, 0, 0) == 1577836800u);
  CHECK(lpwan_gnss_utc_to_unix(2024, 2, 29, 12, 0, 0) == 1709208000u); /* leap day */
  CHECK(lpwan_gnss_utc_to_unix(2024, 3, 1, 0, 0, 0) == 1709251200u);
  CHECK(lpwan_gnss_utc_to_unix(2025, 3, 1, 0, 0, 0) == 1740787200u);
  CHECK(lpwan_gnss_utc_to_unix(2099, 12, 31, 23, 59, 59) == 4102444799u);
  CHECK(lpwan_gnss_utc_to_unix(2026, 11, 18, 10, 20, 30) == 1794997230u);
}

static void test_utc_invalid(void) {
  CHECK(lpwan_gnss_utc_to_unix(2019, 12, 31, 23, 59, 59) == 0); /* before the earliest date of a receiver clock */
  CHECK(lpwan_gnss_utc_to_unix(2100, 1, 1, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 0, 1, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 13, 1, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 1, 0, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 2, 29, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2024, 2, 30, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 4, 31, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 1, 32, 0, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 6, 1, 24, 0, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 6, 1, 0, 60, 0) == 0);
  CHECK(lpwan_gnss_utc_to_unix(2026, 6, 1, 0, 0, 60) == 0);
  CHECK(lpwan_gnss_utc_to_unix(0, 0, 0, 0, 0, 0) == 0); /* the receiver before it has the time */
}

static void test_convert(void) {
  CHECK(lpwan_gnss_acc_m(0.0f) == 0);
  CHECK(lpwan_gnss_acc_m(-1.0f) == 0);
  CHECK(lpwan_gnss_acc_m(NAN) == 0);
  CHECK(lpwan_gnss_acc_m(0.2f) == 1); /* rounded up: never better than it is */
  CHECK(lpwan_gnss_acc_m(3.0f) == 3);
  CHECK(lpwan_gnss_acc_m(3.01f) == 4);
  CHECK(lpwan_gnss_acc_m(1e9f) == 65535u);
  CHECK(lpwan_gnss_deg_e7(47.3764, 90) == 473764000);
  CHECK(lpwan_gnss_deg_e7(-8.5476, 180) == -85476000);
  CHECK(lpwan_gnss_deg_e7(0.0, 90) == 0);
  CHECK(lpwan_gnss_deg_e7(123.0, 90) == 900000000); /* clamped */
  CHECK(lpwan_gnss_deg_e7(-200.0, 180) == -1800000000);
  CHECK(lpwan_gnss_deg_e7(NAN, 90) == 0);
}

/* ---- the messages over the link ---- */

static struct link_slave slv;
static struct lpwan_master mst;
static lpwan_sm_t slave_sm;
static lpwan_sm_t master_sm;
static const uint8_t *armed_tx;
static uint8_t *armed_rx;
static bool busy_pin = true;
static bool event_pin;
static uint32_t clock_ms;

/* the model of the shield app (service.c): a fix, its admission, the refusals while it runs */
static bool old_shield; /* an app without the GNSS: it does not know the tags */
static bool shield_has_gnss = true;
static bool fix_running;
static bool dfu_holds; /* a shield update holds the link */
static uint32_t fix_seq;
static uint32_t fix_timeout_s;
static int fix_starts;
static sensei_GnssFixResult events_result;
static int result_events;
static int other_events;

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
  bool is_gnss =
      req->which_body == sensei_LpwanRequest_gnss_fix_tag || req->which_body == sensei_LpwanRequest_gnss_cancel_tag;

  (void)u;
  r.seq = req->seq;
  if (old_shield && is_gnss) {
    r.result = sensei_LpwanResult_LPWAN_UNSUPPORTED; /* what an app before the GNSS answers to a tag it does not know */
  } else {
    r.result = lpwan_admit_request(&slave_sm, req, dfu_holds, fix_running); /* service.c calls the same function */
  }
  if (r.result == sensei_LpwanResult_LPWAN_OK) {
    if (req->which_body == sensei_LpwanRequest_get_version_tag) {
      r.which_payload = sensei_LpwanResponse_version_tag;
      r.version.has_firmware = true;
      r.version.gnss = shield_has_gnss && !old_shield;
    } else if (req->which_body == sensei_LpwanRequest_get_status_tag) {
      r.which_payload = sensei_LpwanResponse_status_tag;
      if (shield_has_gnss && !old_shield) {
        r.status.has_gnss_attempts = r.status.has_gnss_fixes = r.status.has_gnss_on_ms = true;
        r.status.gnss_attempts = 7;
        r.status.gnss_fixes = 5;
        r.status.gnss_on_ms = 123456;
      }
    } else if (req->which_body == sensei_LpwanRequest_gnss_fix_tag) {
      uint32_t t;

      if (!shield_has_gnss) {
        r.result = sensei_LpwanResult_LPWAN_UNSUPPORTED;
      } else if (!lpwan_gnss_timeout(req->gnss_fix.timeout_s, &t)) {
        r.result = sensei_LpwanResult_LPWAN_INVALID;
      } else {
        fix_running = true;
        fix_seq = req->seq;
        fix_timeout_s = t;
        fix_starts++;
      }
    } else if (req->which_body == sensei_LpwanRequest_gnss_cancel_tag) {
      /* the shield ends the fix and queues the result at its own time: see end_fix() */
    }
  }
  link_slave_respond(s, &r);
}

static int m_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  (void)c;
  if (!armed_tx || busy_pin) {
    return -EBUSY;
  }
  memcpy(armed_rx, tx, len);
  memcpy(rx, armed_tx, len);
  busy_pin = true;
  armed_tx = NULL;
  clock_ms += 2;
  link_slave_done(&slv, 0);
  return 0;
}
static int m_ready(void *c, uint32_t t) {
  (void)c;
  (void)t;
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
  if (e->which_body == sensei_LpwanEvent_gnss_result_tag) {
    events_result = e->gnss_result;
    result_events++;
  } else {
    other_events++;
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
  old_shield = false;
  shield_has_gnss = true;
  fix_running = false;
  dfu_holds = false;
  fix_seq = fix_timeout_s = 0;
  fix_starts = result_events = other_events = 0;
  memset(&events_result, 0, sizeof(events_result));
  clock_ms = 0;
  armed_tx = NULL;
  busy_pin = true;
  event_pin = false;
  link_slave_start(&slv);
}

static int do_req(sensei_LpwanRequest *rq, sensei_LpwanResponse *rs) { return lpwan_master_request(&mst, rq, rs, 100); }

/* The shield ends the fix: the running flag first, then the event (as gnss.c does). */
static int end_fix(sensei_GnssFixStatus status) {
  sensei_LpwanEvent ev = sensei_LpwanEvent_init_zero;

  fix_running = false;
  ev.which_body = sensei_LpwanEvent_gnss_result_tag;
  ev.gnss_result.seq = fix_seq;
  ev.gnss_result.status = status;
  if (status == sensei_GnssFixStatus_GNSS_FIX_OK) {
    ev.gnss_result.lat_e7 = 473764000;
    ev.gnss_result.lon_e7 = -85476000;
    ev.gnss_result.alt_m = 523;
    ev.gnss_result.acc_m = 7;
    ev.gnss_result.sats = 9;
    ev.gnss_result.unix_time = 1780272000u;
    ev.gnss_result.ttff_ms = 31500;
    ev.gnss_result.age_ms = 1250;
  }
  return link_slave_push_event(&slv, &ev);
}

static void req_fix(uint32_t timeout_s, sensei_LpwanResponse *rs) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  rq.which_body = sensei_LpwanRequest_gnss_fix_tag;
  rq.gnss_fix.timeout_s = timeout_s;
  CHECK(do_req(&rq, rs) == 0);
}

static void test_fix_roundtrip(void) {
  sensei_LpwanResponse rs;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  reset_sim();
  /* the version says the shield has the GNSS; the status carries the counters */
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.which_payload == sensei_LpwanResponse_version_tag && rs.version.gnss);
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.which_payload == sensei_LpwanResponse_status_tag);
  CHECK(rs.status.has_gnss_attempts && rs.status.gnss_attempts == 7 && rs.status.gnss_fixes == 5 &&
        rs.status.gnss_on_ms == 123456);

  /* a fix: answered at once, the result is an event */
  req_fix(0, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK && fix_running && fix_timeout_s == LPWAN_GNSS_TIMEOUT_DEFAULT_S);
  CHECK(fix_seq == rs.seq);
  CHECK(lpwan_master_fetch_events(&mst, 4) == 0 && result_events == 0);
  /* one at a time */
  req_fix(30, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_BUSY && fix_starts == 1);
  /* LTE connect and LTE configure wait for the fix, LoRa does not (the same function as the shield,
   * lpwan_admit_request) */
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lte_connect_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);
  CHECK(slave_sm.state == LPWAN_ST_IDLE); /* nothing started */
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);

  /* the result */
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_OK) == 0);
  CHECK(event_pin);
  CHECK(lpwan_master_fetch_events(&mst, 4) == 1 && result_events == 1);
  CHECK(events_result.seq == fix_seq && events_result.status == sensei_GnssFixStatus_GNSS_FIX_OK);
  CHECK(events_result.lat_e7 == 473764000 && events_result.lon_e7 == -85476000 && events_result.alt_m == 523);
  CHECK(events_result.acc_m == 7 && events_result.sats == 9 && events_result.unix_time == 1780272000u);
  CHECK(events_result.ttff_ms == 31500 && events_result.age_ms == 1250);
  /* the next fix may start */
  req_fix(10, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK && fix_starts == 2 && fix_timeout_s == 10);
}

static void test_fix_failures(void) {
  sensei_LpwanResponse rs;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  reset_sim();
  req_fix(901, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_INVALID && !fix_running);

  /* timeout and blocked arrive as events with their status and no position */
  req_fix(20, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_BLOCKED) == 0);
  CHECK(lpwan_master_fetch_events(&mst, 4) == 1);
  CHECK(events_result.status == sensei_GnssFixStatus_GNSS_FIX_BLOCKED && events_result.lat_e7 == 0 &&
        events_result.sats == 0);
  req_fix(20, &rs);
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_TIMEOUT) == 0);
  CHECK(lpwan_master_fetch_events(&mst, 4) == 1 && events_result.status == sensei_GnssFixStatus_GNSS_FIX_TIMEOUT);

  /* a cancel is answered, the cancelled result is the event */
  req_fix(20, &rs);
  rq.which_body = sensei_LpwanRequest_gnss_cancel_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_CANCELLED) == 0);
  CHECK(lpwan_master_fetch_events(&mst, 4) == 1 && events_result.status == sensei_GnssFixStatus_GNSS_FIX_CANCELLED);
  /* a cancel with no fix running is harmless */
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);

  /* LTE busy: the fix is refused and the connect goes on */
  reset_sim();
  slave_sm.tech = LPWAN_TECH_LTE;
  slave_sm.state = LPWAN_ST_SENDING;
  req_fix(20, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_BUSY && !fix_running);
  slave_sm.state = LPWAN_ST_CONNECTED; /* idle and registered: the modem time-shares */
  req_fix(20, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK && fix_running);
  /* an LTE send during the fix goes through: the modem gives LTE priority */
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_send_tag;
  rq.send.radio = sensei_Radio_RADIO_LTEM;
  rq.send.payload.size = 4;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  CHECK(slave_sm.state == LPWAN_ST_SENDING && fix_running);
  /* the release is refused until the fix has ended */
  slave_sm.state = LPWAN_ST_CONNECTED;
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lte_disconnect_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY && slave_sm.state == LPWAN_ST_CONNECTED);
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_TIMEOUT) == 0);
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK && slave_sm.state == LPWAN_ST_DISCONNECTING);
}

static void test_dfu_and_fix(void) {
  sensei_LpwanResponse rs;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  /* an update does not start under a fix; the fix can still be cancelled while an update holds the link */
  reset_sim();
  req_fix(30, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_OK && fix_running);
  rq.which_body = sensei_LpwanRequest_dfu_begin_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_BUSY);
  dfu_holds = true;
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_gnss_cancel_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);
  req_fix(30, &rs); /* no new fix during the update */
  CHECK(rs.result == sensei_LpwanResult_LPWAN_BUSY && fix_starts == 1);
}

static void test_time(void) {
  /* the receiver's milliseconds round the second */
  CHECK(lpwan_gnss_utc_to_unix_ms(2026, 6, 1, 0, 0, 0, 0) == 1780272000u);
  CHECK(lpwan_gnss_utc_to_unix_ms(2026, 6, 1, 0, 0, 0, 499) == 1780272000u);
  CHECK(lpwan_gnss_utc_to_unix_ms(2026, 6, 1, 0, 0, 0, 500) == 1780272001u);
  CHECK(lpwan_gnss_utc_to_unix_ms(2026, 6, 1, 23, 59, 59, 999) == 1780358400u); /* into the next day */
  CHECK(lpwan_gnss_utc_to_unix_ms(2026, 6, 1, 0, 0, 0, 1000) == 0);
  CHECK(lpwan_gnss_utc_to_unix_ms(0, 0, 0, 0, 0, 0, 0) == 0); /* no time: stays 0, however long the ms */
  CHECK(lpwan_gnss_utc_to_unix_ms(0, 0, 0, 0, 0, 0, 900) == 0);
  /* the time of the fix moved to now: the age at the shield, the delay at the client */
  CHECK(lpwan_gnss_time_now(1780272000u, 0, 0) == 1780272000u);
  CHECK(lpwan_gnss_time_now(1780272000u, 1250, 0) == 1780272001u);
  CHECK(lpwan_gnss_time_now(1780272000u, 1250, 300) == 1780272002u); /* 1.55 s */
  CHECK(lpwan_gnss_time_now(1780272000u, 400, 99) == 1780272000u);
  CHECK(lpwan_gnss_time_now(1780272000u, 400, 100) == 1780272001u);
  CHECK(lpwan_gnss_time_now(1780272000u, 3600000u, 0) == 1780272000u + 3600u);
  CHECK(lpwan_gnss_time_now(0, 5000, 5000) == 0);                     /* not known: not made up */
  CHECK(lpwan_gnss_time_now(UINT32_MAX - 1, 10000, 0) == UINT32_MAX); /* no wrap */
}

static void test_old_shield(void) {
  sensei_LpwanResponse rs;
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;

  /* A shield from before the feature: it does not know the tags (UNSUPPORTED), reports no GNSS and no counters.
   * A pairing with it must not break: get_version and get_status still work, other requests are unaffected. */
  reset_sim();
  old_shield = true;
  rq.which_body = sensei_LpwanRequest_get_version_tag;
  CHECK(do_req(&rq, &rs) == 0 && !rs.version.gnss);
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  CHECK(do_req(&rq, &rs) == 0 && !rs.status.has_gnss_attempts);
  req_fix(20, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_UNSUPPORTED && !fix_running && fix_starts == 0);
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_gnss_cancel_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_UNSUPPORTED);
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_lora_join_tag;
  CHECK(do_req(&rq, &rs) == 0 && rs.result == sensei_LpwanResult_LPWAN_OK);

  /* a shield app built without the feature answers the same way */
  reset_sim();
  shield_has_gnss = false;
  req_fix(20, &rs);
  CHECK(rs.result == sensei_LpwanResult_LPWAN_UNSUPPORTED);
}

static void test_result_keeps_its_slot(void) {
  sensei_LpwanEvent dl = sensei_LpwanEvent_init_zero;
  sensei_LpwanResponse rs;

  /* downlinks and time cannot fill the slots that a state change or the result of a fix needs */
  reset_sim();
  req_fix(20, &rs);
  dl.which_body = sensei_LpwanEvent_downlink_rx_tag;
  dl.downlink_rx.radio = sensei_Radio_RADIO_LTEM;
  dl.downlink_rx.payload.size = 3;
  for (int i = 0; i < 6; i++) {
    (void)link_slave_push_event(&slv, &dl);
  }
  CHECK(link_slave_events_pending(&slv) == LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED);
  CHECK(end_fix(sensei_GnssFixStatus_GNSS_FIX_OK) == 0); /* still queued */
  CHECK(link_slave_events_pending(&slv) == LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED + 1);
  CHECK(lpwan_master_fetch_events(&mst, 16) == (int)(LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED + 1));
  CHECK(result_events == 1 && other_events == (int)(LINK_SLAVE_EVENT_SLOTS - LINK_SLAVE_EVENT_RESERVED));
}

int main(void) {
  test_timeout();
  test_admit();
  test_refuses();
  test_allowed_during_dfu();
  test_admit_request();
  test_end_status();
  test_utc();
  test_utc_invalid();
  test_convert();
  test_fix_roundtrip();
  test_fix_failures();
  test_dfu_and_fix();
  test_time();
  test_old_shield();
  test_result_keeps_its_slot();
  printf("%s\n", check_failures ? "FAILED" : "ok");
  return check_failures ? 1 : 0;
}
