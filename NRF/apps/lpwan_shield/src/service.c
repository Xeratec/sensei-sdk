/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 * - Serge Wüest <swueest@ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

/**
 * @file service.c
 * @brief Request dispatcher and event hub. The link thread calls on_request(); it checks the
 *        request against the global state machine and starts the backend, which reports by events.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dfu.h"
#include "diag.h"
#ifdef CONFIG_LPWAN_SHIELD_GNSS
#include "gnss.h"
#endif
#include "link.h"
#include "lora.h"
#include "lte.h"
#include "sensei_lpwan/lpwan_gnss.h"
#include "sensei_lpwan/lpwan_lora.h"
#include "service.h"

LOG_MODULE_REGISTER(service, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static lpwan_sm_t sm;
static K_MUTEX_DEFINE(sm_lock);

/*
 * A send that gets no tx_done is ended with tx_done(failed), so the state machine leaves SENDING (the backend or
 * the modem may hang). The limit is the time the backend expects (LoRa: the duty-cycle wait of the LR1121, the
 * transmissions and their receive windows) plus 5 s, at most LPWAN_SEND_MAX_MS (10 min); without an estimate
 * CONFIG_LPWAN_SHIELD_SEND_TIMEOUT_S. The master waits longer.
 *
 * While the LR1121 still holds the frame the send is not failed at the limit: the frame would go out later and a
 * retry would send it twice. The watchdog looks again, and resets the LR1121 after twice the limit (the frame is
 * dropped, the send ends with tx_done(failed)). Each send has one tx_done: the first one wins. A real result that
 * comes when no send waits for it is kept for the status (late_tx_seq), once.
 * The send in flight is in `txt` (under sm_lock). The watchdog acts only for the send it was armed for.
 */
#define SEND_WD_EXTRA_MS 5000U

static struct k_work_delayable send_wd;
static struct lpwan_txtrack txt;

static bool backend_holds_frame(uint32_t seq) {
#ifdef CONFIG_LPWAN_SHIELD_LORA
  return sm.tech == LPWAN_TECH_LORA && lora_holds_send(seq);
#else
  ARG_UNUSED(seq);
  return false;
#endif
}

static void send_wd_fn(struct k_work *w) {
  enum lpwan_wd_action act;
  uint32_t seq, rearm_ms;

  ARG_UNUSED(w);
  k_mutex_lock(&sm_lock, K_FOREVER);
  act = lpwan_txt_wd(&txt, k_uptime_get_32(), backend_holds_frame(txt.open_seq), &seq, &rearm_ms);
  k_mutex_unlock(&sm_lock);
  switch (act) {
  case LPWAN_WD_NONE:
    break;
  case LPWAN_WD_WAIT:
    if (rearm_ms > 0) {
      LOG_WRN("send %u: the LR1121 still holds the frame", (unsigned)seq);
      (void)k_work_reschedule(&send_wd, K_MSEC(rearm_ms));
    }
    break;
  case LPWAN_WD_FAIL:
    LOG_WRN("send %u: no result from the backend", (unsigned)seq);
    svc_emit_tx_done(seq, false, 0, 0);
    break;
  case LPWAN_WD_RESET:
#ifdef CONFIG_LPWAN_SHIELD_LORA
    if (!lora_reset_stuck()) {
      (void)k_work_reschedule(&send_wd, K_SECONDS(1)); /* the worker queue is full: try again */
    }
#endif
    break;
  }
}

/* ---- events ---- */

/* The state machine moves only if the event was queued: a state change the master never hears of would
 * leave the two machines apart. State events have reserved queue slots (link_slave.h), so this is rare. */
void svc_emit(const sensei_LpwanEvent *evt) {
  k_mutex_lock(&sm_lock, K_FOREVER);
  if (evt->which_body == sensei_LpwanEvent_tx_done_tag && !lpwan_txt_wants(&txt, evt->tx_done.seq)) {
    LOG_WRN("tx_done for seq %u: no send waits for it, kept for the status", (unsigned)evt->tx_done.seq);
    lpwan_txt_late(&txt, evt->tx_done.seq, evt->tx_done.ok);
  } else if (link_push_event(evt) == 0) {
    lpwan_sm_event(&sm, evt);
    if (evt->which_body == sensei_LpwanEvent_tx_done_tag) {
      lpwan_txt_done(&txt, evt->tx_done.seq);
      (void)k_work_cancel_delayable(&send_wd);
    }
  } else {
    LOG_WRN("event %u dropped (queue full)", (unsigned)evt->which_body);
  }
  k_mutex_unlock(&sm_lock);
}

void svc_emit_radio(pb_size_t tag, sensei_Radio radio) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = tag;
  e.joined.radio = radio; /* joined, connected and disconnected share the layout */
  svc_emit(&e);
}

static void emit_tx_done(uint32_t seq, bool ok, uint32_t attempts, uint32_t next_max_payload, bool acked,
                         bool unconfirmed) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = sensei_LpwanEvent_tx_done_tag;
  e.tx_done.seq = seq;
  e.tx_done.ok = ok;
  e.tx_done.attempts = attempts;
  e.tx_done.next_max_payload = next_max_payload;
  e.tx_done.acked = acked;
  e.tx_done.unconfirmed = unconfirmed;
  svc_emit(&e);
}

void svc_emit_tx_done(uint32_t seq, bool ok, uint32_t attempts, uint32_t next_max_payload) {
  emit_tx_done(seq, ok, attempts, next_max_payload, false, false);
}

void svc_emit_tx_done_ack(uint32_t seq, bool ok, uint32_t attempts, bool acked, bool unconfirmed) {
  emit_tx_done(seq, ok, attempts, 0, acked, unconfirmed);
}

void svc_emit_downlink(sensei_Radio radio, uint32_t port, const uint8_t *data, size_t len) {
  static sensei_LpwanEvent e; /* 1 kB: not on a small stack; callers are serialised per backend */

  if (len > sizeof(e.downlink_rx.payload.bytes)) {
    len = sizeof(e.downlink_rx.payload.bytes);
  }
  memset(&e, 0, sizeof(e));
  e.which_body = sensei_LpwanEvent_downlink_rx_tag;
  e.downlink_rx.radio = radio;
  e.downlink_rx.port = port;
  e.downlink_rx.payload.size = (pb_size_t)len;
  memcpy(e.downlink_rx.payload.bytes, data, len);
  svc_emit(&e);
}

void svc_emit_time(sensei_Radio radio, uint32_t unix_s) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = sensei_LpwanEvent_network_time_tag;
  e.network_time.unix_time = unix_s;
  e.network_time.radio = radio;
  svc_emit(&e);
}

void svc_emit_gnss_result(const sensei_GnssFixResult *r) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = sensei_LpwanEvent_gnss_result_tag;
  e.gnss_result = *r;
  svc_emit(&e);
}

void svc_emit_error(sensei_LpwanResult code, const char *msg) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = sensei_LpwanEvent_error_tag;
  e.error.code = code;
  strncpy(e.error.message, msg ? msg : "", sizeof(e.error.message) - 1);
  svc_emit(&e);
}

lpwan_state_t svc_state(void) { return sm.state; }

lpwan_tech_t svc_tech(void) { return sm.tech; }

/* ---- requests ---- */

static sensei_LpwanResult backend_off(void) { return sensei_LpwanResult_LPWAN_UNSUPPORTED; }

static void fill_version(sensei_LpwanResponse *r) {
  r->which_payload = sensei_LpwanResponse_version_tag;
  r->version.has_firmware = true;
  r->version.firmware.major = APP_VERSION_MAJOR;
  r->version.firmware.minor = APP_VERSION_MINOR;
  r->version.firmware.patch = APP_PATCHLEVEL;
  r->version.protocol = LPWAN_PROTOCOL_VERSION;
  dfu_fill_version(&r->version);
  r->version.gnss = IS_ENABLED(CONFIG_LPWAN_SHIELD_GNSS);
  r->version.modem[0] = '\0';
#ifdef CONFIG_LPWAN_SHIELD_LORA
  lora_modem_string(r->version.modem, sizeof(r->version.modem));
#endif
#ifdef CONFIG_LPWAN_SHIELD_LTE
  lte_modem_string(r->version.lte_modem, sizeof(r->version.lte_modem));
  // cppcheck-suppress knownConditionTrueFalse ; set by lora_modem_string() when LoRa is built
  if (r->version.modem[0] == '\0') {
    snprintf(r->version.modem, sizeof(r->version.modem), "%s", r->version.lte_modem);
  }
#endif
}

static void fill_status(sensei_LpwanResponse *r) {
  r->which_payload = sensei_LpwanResponse_status_tag;
  memset(&r->status, 0, sizeof(r->status));
  r->status.dropped_events = link_dropped_events();
  diag_fill_status(&r->status);
#ifdef CONFIG_LPWAN_SHIELD_LTE
  lte_fill_status(&r->status);
#endif
#ifdef CONFIG_LPWAN_SHIELD_LORA
  lora_fill_status(&r->status);
#endif
#ifdef CONFIG_LPWAN_SHIELD_GNSS
  gnss_fill_status(&r->status);
#endif
  k_mutex_lock(&sm_lock, K_FOREVER);
  if (lpwan_txt_take_late(&txt, &r->status.late_tx_seq, &r->status.late_tx_ok)) {
    LOG_INF("status: late result of send %u (%s)", (unsigned)r->status.late_tx_seq,
            r->status.late_tx_ok ? "ok" : "failed");
  }
  k_mutex_unlock(&sm_lock);
}

/* What a send tells the master (SendInfo). */
struct send_answer {
  uint32_t expected_ms;
  uint32_t next_max;
  uint32_t queued_seq;
};

/* Run a backend action for a request the state machine accepted. If it refuses, undo the state. */
static sensei_LpwanResult run(const sensei_LpwanRequest *req, struct send_answer *ans) {
  switch (req->which_body) {
  case sensei_LpwanRequest_lte_configure_tag:
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_configure(&req->lte_configure);
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lte_connect_tag:
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_connect();
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lora_configure_tag:
#ifdef CONFIG_LPWAN_SHIELD_LORA
    return lora_configure(&req->lora_configure);
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lora_provision_tag:
#ifdef CONFIG_LPWAN_SHIELD_LORA
    return lora_provision(&req->lora_provision);
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lora_join_tag:
#ifdef CONFIG_LPWAN_SHIELD_LORA
    return lora_join();
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_send_tag: {
    const sensei_Send *s = &req->send;

    if (s->radio == sensei_Radio_RADIO_LORA) {
#ifdef CONFIG_LPWAN_SHIELD_LORA
      {
        struct lora_send_info li;
        sensei_LpwanResult r = lora_send(req->seq, s->port, s->confirmed, s->payload.bytes, s->payload.size, &li);

        ans->expected_ms = li.expected_ms;
        ans->next_max = li.next_max;
        ans->queued_seq = li.queued_seq;
        return r;
      }
#else
      return backend_off();
#endif
    }
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_send(req->seq, s->port, s->payload.bytes, s->payload.size, s->rai);
#else
    return backend_off();
#endif
  }
  case sensei_LpwanRequest_get_time_tag:
    if (sm.tech == LPWAN_TECH_LORA) {
#ifdef CONFIG_LPWAN_SHIELD_LORA
      return lora_request_time();
#endif
    } else if (sm.tech == LPWAN_TECH_LTE) {
#ifdef CONFIG_LPWAN_SHIELD_LTE
      return lte_request_time();
#endif
    }
    return backend_off();
  case sensei_LpwanRequest_gnss_fix_tag:
#ifdef CONFIG_LPWAN_SHIELD_GNSS
    return gnss_start(req->seq, req->gnss_fix.timeout_s); /* admitted by lpwan_admit_request() */
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_gnss_cancel_tag:
#ifdef CONFIG_LPWAN_SHIELD_GNSS
    return gnss_cancel();
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lte_eval_tag:
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_eval();
#else
    return backend_off();
#endif
  case sensei_LpwanRequest_lte_disconnect_tag:
    if (sm.tech == LPWAN_TECH_LORA) {
#ifdef CONFIG_LPWAN_SHIELD_LORA
      return lora_leave();
#endif
    } else if (sm.tech == LPWAN_TECH_LTE) {
#ifdef CONFIG_LPWAN_SHIELD_LTE
      return lte_disconnect();
#endif
    }
    /* Nothing active: report disconnected so both state machines settle. */
    svc_emit_radio(sensei_LpwanEvent_disconnected_tag, sensei_Radio_RADIO_LORA);
    return sensei_LpwanResult_LPWAN_OK;
  case sensei_LpwanRequest_sleep_tag:
    if (sm.tech == LPWAN_TECH_LORA) {
#ifdef CONFIG_LPWAN_SHIELD_LORA
      return lora_sleep(req->sleep.deep);
#endif
    }
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_sleep(req->sleep.deep);
#else
    return sensei_LpwanResult_LPWAN_OK;
#endif
#ifndef CONFIG_LPWAN_SHIELD_DFU
  case sensei_LpwanRequest_dfu_begin_tag:
  case sensei_LpwanRequest_dfu_chunk_tag:
  case sensei_LpwanRequest_dfu_finish_tag:
  case sensei_LpwanRequest_dfu_apply_tag:
  case sensei_LpwanRequest_dfu_abort_tag:
  case sensei_LpwanRequest_dfu_status_tag:
  case sensei_LpwanRequest_dfu_confirm_tag:
    return backend_off();
#endif
  default:
    return sensei_LpwanResult_LPWAN_OK;
  }
}

static void fill_send_answer(sensei_LpwanResponse *rsp, const struct send_answer *a) {
  rsp->which_payload = sensei_LpwanResponse_send_tag;
  rsp->send.expected_ms = a->expected_ms;
  rsp->send.next_max_payload = a->next_max;
  rsp->send.queued_seq = a->queued_seq;
}

static bool is_dfu_request(const sensei_LpwanRequest *req) {
  return req->which_body >= sensei_LpwanRequest_dfu_begin_tag && req->which_body <= sensei_LpwanRequest_dfu_confirm_tag;
}

static void on_request(struct link_slave *s, const sensei_LpwanRequest *req, void *user) {
  sensei_LpwanResponse rsp = sensei_LpwanResponse_init_zero;
  sensei_LpwanResult res;
  bool is_send = req->which_body == sensei_LpwanRequest_send_tag;
  struct send_answer ans = {0};

  ARG_UNUSED(s);
  ARG_UNUSED(user);
  rsp.seq = req->seq;

  k_mutex_lock(&sm_lock, K_FOREVER);
  lpwan_sm_t before = sm;
  lpwan_sm_t admitted;

  /* An update holds the link (no radio work meanwhile: the flash, the modem and the CPU are busy); a fix and what
   * changes the modem's functional mode exclude each other. The order of the checks is lpwan_admit_request(). */
#ifdef CONFIG_LPWAN_SHIELD_GNSS
  res = lpwan_admit_request(&sm, req, dfu_holds_link(), gnss_active());
#else
  res = lpwan_admit_request(&sm, req, dfu_holds_link(), false);
#endif
  admitted = sm;
  if (res == sensei_LpwanResult_LPWAN_OK) {
    k_mutex_unlock(&sm_lock);
    if (req->which_body == sensei_LpwanRequest_get_version_tag) {
      fill_version(&rsp);
    } else if (req->which_body == sensei_LpwanRequest_get_status_tag) {
      fill_status(&rsp);
    } else if (req->which_body == sensei_LpwanRequest_diag_ack_tag) {
      diag_ack();
    } else if (is_dfu_request(req)) {
#ifdef CONFIG_LPWAN_SHIELD_DFU
      res = dfu_handle(req, &rsp);
#else
      res = backend_off();
#endif
    } else {
      if (is_send) {
        k_mutex_lock(&sm_lock, K_FOREVER);
        lpwan_txt_open(&txt, req->seq); /* the backend may answer at once */
        k_mutex_unlock(&sm_lock);
      }
      res = run(req, &ans);
      if (res != sensei_LpwanResult_LPWAN_OK) {
        /* The backend refused: undo what the admission started, unless events moved it on. */
        k_mutex_lock(&sm_lock, K_FOREVER);
        lpwan_sm_revert(&sm, &before, &admitted);
        if (is_send) {
          lpwan_txt_refused(&txt, req->seq);
        }
        k_mutex_unlock(&sm_lock);
        if (is_send && (ans.next_max != 0 || ans.expected_ms != 0 || ans.queued_seq != 0)) {
          fill_send_answer(&rsp, &ans); /* too long: what fits; busy: when */
        }
      } else if (is_send) {
        /* Start the watchdog unless the result is in already (tx_done during run()). */
        uint32_t limit =
            lpwan_send_limit_ms(ans.expected_ms, CONFIG_LPWAN_SHIELD_SEND_TIMEOUT_S * 1000U, SEND_WD_EXTRA_MS);

        k_mutex_lock(&sm_lock, K_FOREVER);
        if (lpwan_txt_armed(&txt, req->seq, k_uptime_get_32(), limit)) {
          (void)k_work_reschedule(&send_wd, K_MSEC(limit));
        }
        k_mutex_unlock(&sm_lock);
        fill_send_answer(&rsp, &ans);
      }
    }
  } else {
    k_mutex_unlock(&sm_lock);
    if (is_send && res == sensei_LpwanResult_LPWAN_BUSY && sm.tech == LPWAN_TECH_LORA) {
      /* A send is open (the client gave up on it, or lost track): say which frame the LR1121 holds. */
#ifdef CONFIG_LPWAN_SHIELD_LORA
      uint32_t held_seq, left_ms;

      if (lora_held(&held_seq, &left_ms)) {
        ans.queued_seq = held_seq;
        ans.expected_ms = left_ms;
        fill_send_answer(&rsp, &ans);
      }
#endif
    }
  }
  rsp.result = res;
  LOG_DBG("request %u tag %u -> %d (state %s, tech %s)", (unsigned)req->seq, (unsigned)req->which_body, (int)res,
          lpwan_state_name(sm.state), lpwan_tech_name(sm.tech));
  link_respond(&rsp);
}

int service_start(void) {
  lpwan_sm_init(&sm);
  lpwan_txt_init(&txt);
  k_work_init_delayable(&send_wd, send_wd_fn);
  return link_start(on_request, NULL);
}
