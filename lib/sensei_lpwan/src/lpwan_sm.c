/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

#include "sensei_lpwan/lpwan_link.h"

void lpwan_sm_init(lpwan_sm_t *sm) {
  sm->state = LPWAN_ST_IDLE;
  sm->tech = LPWAN_TECH_NONE;
  sm->send_seq = 0;
}

lpwan_tech_t lpwan_tech_of_radio(sensei_Radio r) {
  switch (r) {
  case sensei_Radio_RADIO_LORA:
    return LPWAN_TECH_LORA;
  case sensei_Radio_RADIO_LTEM:
  case sensei_Radio_RADIO_NBIOT:
    return LPWAN_TECH_LTE;
  case sensei_Radio_RADIO_BLE:
    return LPWAN_TECH_BLE;
  default:
    return LPWAN_TECH_NONE;
  }
}

sensei_LpwanResult lpwan_sm_admit(lpwan_sm_t *sm, const sensei_LpwanRequest *req) {
  switch (req->which_body) {
  case sensei_LpwanRequest_get_version_tag:
  case sensei_LpwanRequest_get_status_tag:
    return sensei_LpwanResult_LPWAN_OK;

  case sensei_LpwanRequest_lte_configure_tag:
  case sensei_LpwanRequest_lora_configure_tag:
  case sensei_LpwanRequest_lora_provision_tag:
    return sm->state == LPWAN_ST_IDLE ? sensei_LpwanResult_LPWAN_OK : sensei_LpwanResult_LPWAN_BUSY;

  case sensei_LpwanRequest_lte_connect_tag:
  case sensei_LpwanRequest_lora_join_tag: {
    lpwan_tech_t want = req->which_body == sensei_LpwanRequest_lte_connect_tag ? LPWAN_TECH_LTE : LPWAN_TECH_LORA;

    if (sm->state == LPWAN_ST_ERROR) {
      return sensei_LpwanResult_LPWAN_FAILED;
    }
    if (sm->tech != LPWAN_TECH_NONE && sm->tech != want) {
      return sensei_LpwanResult_LPWAN_BUSY; /* another radio is active */
    }
    if (sm->state != LPWAN_ST_IDLE) {
      return sensei_LpwanResult_LPWAN_BUSY;
    }
    sm->state = LPWAN_ST_CONNECTING;
    sm->tech = want;
    return sensei_LpwanResult_LPWAN_OK;
  }

  case sensei_LpwanRequest_send_tag: {
    lpwan_tech_t t = lpwan_tech_of_radio(req->send.radio);

    if (t != LPWAN_TECH_LTE && t != LPWAN_TECH_LORA) {
      return sensei_LpwanResult_LPWAN_INVALID;
    }
    if (sm->state == LPWAN_ST_ERROR) {
      return sensei_LpwanResult_LPWAN_FAILED;
    }
    if (sm->tech != t || sm->state == LPWAN_ST_IDLE || sm->state == LPWAN_ST_DISCONNECTING) {
      return t == LPWAN_TECH_LORA ? sensei_LpwanResult_LPWAN_NOT_JOINED : sensei_LpwanResult_LPWAN_NOT_CONNECTED;
    }
    if (sm->state == LPWAN_ST_CONNECTING) {
      return t == LPWAN_TECH_LORA ? sensei_LpwanResult_LPWAN_NOT_JOINED : sensei_LpwanResult_LPWAN_NOT_CONNECTED;
    }
    if (sm->state == LPWAN_ST_SENDING) {
      return sensei_LpwanResult_LPWAN_BUSY;
    }
    sm->state = LPWAN_ST_SENDING;
    sm->send_seq = req->seq;
    return sensei_LpwanResult_LPWAN_OK;
  }

  case sensei_LpwanRequest_get_time_tag:
    if (sm->state == LPWAN_ST_CONNECTED) {
      return sensei_LpwanResult_LPWAN_OK;
    }
    return sm->tech == LPWAN_TECH_LORA ? sensei_LpwanResult_LPWAN_NOT_JOINED : sensei_LpwanResult_LPWAN_NOT_CONNECTED;

  case sensei_LpwanRequest_lte_disconnect_tag:
    if (sm->state == LPWAN_ST_IDLE || sm->state == LPWAN_ST_DISCONNECTING) {
      return sensei_LpwanResult_LPWAN_OK;
    }
    if (sm->state == LPWAN_ST_SENDING) {
      return sensei_LpwanResult_LPWAN_BUSY;
    }
    sm->state = LPWAN_ST_DISCONNECTING;
    return sensei_LpwanResult_LPWAN_OK;

  case sensei_LpwanRequest_sleep_tag:
    if (sm->state == LPWAN_ST_IDLE || sm->state == LPWAN_ST_CONNECTED) {
      return sensei_LpwanResult_LPWAN_OK;
    }
    return sensei_LpwanResult_LPWAN_BUSY;

  default:
    return sensei_LpwanResult_LPWAN_UNSUPPORTED;
  }
}

bool lpwan_event_is_state(const sensei_LpwanEvent *evt) {
  switch (evt->which_body) {
  case sensei_LpwanEvent_joined_tag:
  case sensei_LpwanEvent_connected_tag:
  case sensei_LpwanEvent_disconnected_tag:
  case sensei_LpwanEvent_tx_done_tag:
  case sensei_LpwanEvent_error_tag:
    return true;
  default:
    return false;
  }
}

/* The radio of a joined, connected or disconnected event must be the active technology. */
static bool radio_fits(const lpwan_sm_t *sm, lpwan_tech_t want, sensei_Radio r) {
  lpwan_tech_t t = lpwan_tech_of_radio(r);

  return t == want && (sm->tech == LPWAN_TECH_NONE || sm->tech == t);
}

bool lpwan_sm_event(lpwan_sm_t *sm, const sensei_LpwanEvent *evt) {
  switch (evt->which_body) {
  case sensei_LpwanEvent_joined_tag:
  case sensei_LpwanEvent_connected_tag: {
    bool joined = evt->which_body == sensei_LpwanEvent_joined_tag;
    const sensei_RadioEvent *r = joined ? &evt->joined : &evt->connected;

    if (!radio_fits(sm, joined ? LPWAN_TECH_LORA : LPWAN_TECH_LTE, r->radio)) {
      return false; /* another radio's event, for example a late LoRa join during an LTE connect */
    }
    if (sm->state != LPWAN_ST_CONNECTING && sm->state != LPWAN_ST_IDLE) {
      return false;
    }
    /* IDLE: the LR1121 restored a stored session after a power cycle. */
    sm->state = LPWAN_ST_CONNECTED;
    sm->tech = lpwan_tech_of_radio(r->radio);
    return true;
  }
  case sensei_LpwanEvent_disconnected_tag:
    if (sm->tech != LPWAN_TECH_NONE && lpwan_tech_of_radio(evt->disconnected.radio) != sm->tech) {
      return false; /* not the active technology (a BLE claim is not ended by a LoRa event) */
    }
    sm->state = LPWAN_ST_IDLE;
    sm->tech = LPWAN_TECH_NONE;
    return true;
  case sensei_LpwanEvent_tx_done_tag:
    if (sm->state != LPWAN_ST_SENDING || evt->tx_done.seq != sm->send_seq) {
      return false;
    }
    sm->state = LPWAN_ST_CONNECTED;
    return true;
  case sensei_LpwanEvent_error_tag:
    if (sm->state == LPWAN_ST_CONNECTING) {
      /* Connect or join failed: back to idle, the caller may try again. */
      sm->state = LPWAN_ST_IDLE;
      sm->tech = LPWAN_TECH_NONE;
    } else if (sm->state == LPWAN_ST_SENDING) {
      sm->state = LPWAN_ST_CONNECTED;
    } else if (evt->error.code == sensei_LpwanResult_LPWAN_FAILED && sm->state != LPWAN_ST_IDLE) {
      sm->state = LPWAN_ST_ERROR;
    }
    return true;
  default:
    return true;
  }
}

void lpwan_sm_abort_send(lpwan_sm_t *sm) {
  if (sm->state == LPWAN_ST_SENDING) {
    sm->state = LPWAN_ST_CONNECTED;
  }
}

void lpwan_sm_revert(lpwan_sm_t *sm, const lpwan_sm_t *before, const lpwan_sm_t *admitted) {
  if (sm->state == admitted->state && sm->tech == admitted->tech) {
    *sm = *before;
  }
}

sensei_LpwanResult lpwan_sm_claim_ble(lpwan_sm_t *sm, int claim) {
  if (claim) {
    if (sm->tech != LPWAN_TECH_NONE && sm->tech != LPWAN_TECH_BLE) {
      return sensei_LpwanResult_LPWAN_BUSY;
    }
    sm->tech = LPWAN_TECH_BLE;
  } else if (sm->tech == LPWAN_TECH_BLE) {
    sm->tech = LPWAN_TECH_NONE;
  }
  return sensei_LpwanResult_LPWAN_OK;
}

const char *lpwan_state_name(lpwan_state_t s) {
  static const char *const n[] = {"idle", "connecting", "connected", "sending", "disconnecting", "error"};

  return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

const char *lpwan_tech_name(lpwan_tech_t t) {
  static const char *const n[] = {"none", "lte", "lora", "ble"};

  return (unsigned)t < sizeof(n) / sizeof(n[0]) ? n[t] : "?";
}
