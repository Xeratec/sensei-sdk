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

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "link.h"
#include "lora.h"
#include "lte.h"
#include "service.h"

LOG_MODULE_REGISTER(service, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static lpwan_sm_t sm;
static K_MUTEX_DEFINE(sm_lock);

/* ---- events ---- */

void svc_emit(const sensei_LpwanEvent *evt) {
  k_mutex_lock(&sm_lock, K_FOREVER);
  lpwan_sm_event(&sm, evt);
  k_mutex_unlock(&sm_lock);
  if (link_push_event(evt) != 0) {
    LOG_WRN("event %u dropped (queue full)", (unsigned)evt->which_body);
  }
}

void svc_emit_radio(pb_size_t tag, sensei_Radio radio) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = tag;
  e.joined.radio = radio; /* joined, connected and disconnected share the layout */
  svc_emit(&e);
}

void svc_emit_tx_done(uint32_t seq, bool ok, uint32_t attempts) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;

  e.which_body = sensei_LpwanEvent_tx_done_tag;
  e.tx_done.seq = seq;
  e.tx_done.ok = ok;
  e.tx_done.attempts = attempts;
  svc_emit(&e);
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
  r->version.firmware.major = CONFIG_LPWAN_SHIELD_VERSION_MAJOR;
  r->version.firmware.minor = CONFIG_LPWAN_SHIELD_VERSION_MINOR;
  r->version.firmware.patch = CONFIG_LPWAN_SHIELD_VERSION_PATCH;
  r->version.modem[0] = '\0';
#ifdef CONFIG_LPWAN_SHIELD_LORA
  lora_modem_string(r->version.modem, sizeof(r->version.modem));
#endif
#ifdef CONFIG_LPWAN_SHIELD_LTE
  // cppcheck-suppress knownConditionTrueFalse ; set by lora_modem_string() when LoRa is built
  if (r->version.modem[0] == '\0') {
    lte_modem_string(r->version.modem, sizeof(r->version.modem));
  }
#endif
}

static void fill_status(sensei_LpwanResponse *r) {
  r->which_payload = sensei_LpwanResponse_status_tag;
  memset(&r->status, 0, sizeof(r->status));
#ifdef CONFIG_LPWAN_SHIELD_LTE
  lte_fill_status(&r->status);
#endif
#ifdef CONFIG_LPWAN_SHIELD_LORA
  lora_fill_status(&r->status);
#endif
}

/* Run a backend action for a request the state machine accepted. If it refuses, undo the state. */
static sensei_LpwanResult run(const sensei_LpwanRequest *req) {
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
      return lora_send(req->seq, s->port, s->confirmed, s->payload.bytes, s->payload.size);
#else
      return backend_off();
#endif
    }
#ifdef CONFIG_LPWAN_SHIELD_LTE
    return lte_send(req->seq, s->port, s->payload.bytes, s->payload.size);
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
  default:
    return sensei_LpwanResult_LPWAN_OK;
  }
}

static void on_request(struct link_slave *s, const sensei_LpwanRequest *req, void *user) {
  sensei_LpwanResponse rsp = sensei_LpwanResponse_init_zero;
  sensei_LpwanResult res;

  ARG_UNUSED(s);
  ARG_UNUSED(user);
  rsp.seq = req->seq;

  k_mutex_lock(&sm_lock, K_FOREVER);
  lpwan_state_t before_state = sm.state;
  lpwan_tech_t before_tech = sm.tech;

  res = lpwan_sm_admit(&sm, req);
  if (res == sensei_LpwanResult_LPWAN_OK) {
    k_mutex_unlock(&sm_lock);
    if (req->which_body == sensei_LpwanRequest_get_version_tag) {
      fill_version(&rsp);
    } else if (req->which_body == sensei_LpwanRequest_get_status_tag) {
      fill_status(&rsp);
    } else {
      res = run(req);
      if (res != sensei_LpwanResult_LPWAN_OK) {
        /* The backend refused: undo what the admission started. */
        k_mutex_lock(&sm_lock, K_FOREVER);
        sm.state = before_state;
        sm.tech = before_tech;
        k_mutex_unlock(&sm_lock);
      }
    }
  } else {
    k_mutex_unlock(&sm_lock);
  }
  rsp.result = res;
  LOG_DBG("request %u tag %u -> %d (state %s, tech %s)", (unsigned)req->seq, (unsigned)req->which_body, (int)res,
          lpwan_state_name(sm.state), lpwan_tech_name(sm.tech));
  link_respond(&rsp);
}

int service_start(void) {
  lpwan_sm_init(&sm);
  return link_start(on_request, NULL);
}
