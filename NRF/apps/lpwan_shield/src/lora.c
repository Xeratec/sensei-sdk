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
 * @file lora.c
 * @brief LoRaWAN on the LR1121 with the Semtech Modem-E firmware (vendored host driver).
 *
 * One worker thread owns the LR1121. Commands come from the link thread and from the LR1121 event
 * line (DIO9) through a message queue. States as in the commissioning project: idle, joining,
 * joined, ready, sending. The device class is set after the join; the uplink waits for `ready`.
 *
 * The LR1121 stays in reset until LoRa is first used. After that the Modem-E firmware sleeps on
 * its own. It keeps the LoRaWAN session in its flash: after a shield power cycle the backend
 * restores `ready` from the modem status instead of joining again.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lora.h"
#include "lr1121_modem_bsp.h"
#include "lr1121_modem_lorawan.h"
#include "lr1121_modem_modem.h"
#include "lr1121_modem_system.h"
#include "lr1121_zephyr_hal.h"
#include "sensei_lpwan/lpwan_lora.h"
#include "service.h"

LOG_MODULE_REGISTER(lora, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define ctx (&lr1121_ctx)
#define RC_OK LR1121_MODEM_RESPONSE_CODE_OK

/* Modem-E TX done status */
#define TX_NOT_SENT 0x00
#define TX_UNCONFIRMED 0x01
#define TX_CONFIRMED 0x02

/* GPS epoch (1980-01-06) in Unix seconds, and the GPS-UTC offset in 2026. */
#define GPS_TO_UNIX_S 315964800u
#define GPS_UTC_LEAP_S 18u

enum cmd { CMD_EVENT, CMD_JOIN, CMD_SEND, CMD_TIME, CMD_LEAVE };

struct msg {
  enum cmd cmd;
  uint32_t seq;
  uint8_t port;
  bool confirmed;
  uint8_t len;
  uint8_t data[222];
};

enum lstate { L_OFF, L_IDLE, L_JOINING, L_JOINED, L_READY, L_SENDING };

K_MSGQ_DEFINE(lora_q, sizeof(struct msg), 6, 4);

static sensei_LoraConfigure cfg = {
    .region = sensei_LoraRegion_REGION_EU868,
    .lora_class = sensei_LoraClass_CLASS_A,
    .data_rate = 5,
    .adr = false,
    .confirmed = false,
    .port = 1,
};
static sensei_LoraProvision keys;
static bool keys_dirty;
static volatile bool join_pending; /* a CMD_JOIN is queued and has not started yet */
static volatile enum lstate state = L_OFF;
static bool started;
static unsigned join_failures;
static uint32_t cur_seq;
static bool cur_confirmed;
static char modem_str[24];
static int16_t last_rssi;
static int8_t last_snr;
static struct gpio_callback ev_cb;
static struct k_thread thread;
K_THREAD_STACK_DEFINE(stack, CONFIG_LPWAN_SHIELD_LORA_STACK);

static lr1121_modem_regions_t region_to_lr(sensei_LoraRegion r) {
  switch (r) {
  case sensei_LoraRegion_REGION_US915:
    return LR1121_LORAWAN_REGION_US915;
  case sensei_LoraRegion_REGION_AS923:
    return LR1121_LORAWAN_REGION_AS923_GRP1;
  case sensei_LoraRegion_REGION_AU915:
    return LR1121_LORAWAN_REGION_AU915;
  case sensei_LoraRegion_REGION_KR920:
    return LR1121_LORAWAN_REGION_KR920;
  case sensei_LoraRegion_REGION_IN865:
    return LR1121_LORAWAN_REGION_IN865;
  case sensei_LoraRegion_REGION_RU864:
    return LR1121_LORAWAN_REGION_RU864;
  case sensei_LoraRegion_REGION_EU868:
  default:
    return LR1121_LORAWAN_REGION_EU868;
  }
}

static bool all_same(const uint8_t *p, size_t n, uint8_t v) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != v) {
      return false;
    }
  }
  return true;
}

/* ---- radio setup (after every reset), values from the commissioning project ---- */

static lr1121_modem_response_code_t configure_radio(void) {
  lr1121_modem_response_code_t rc;
  const lr1121_modem_system_rfswitch_cfg_t sw = {
      /* DIO5 (RF_SW_CTRL) and DIO6 (RF_SW_EN) drive the SKY13330 switch of the shield. */
      .enable = 0x03, .standby = 0x00, .rx = 0x03, .tx = 0x02, .tx_hp = 0x02, .tx_hf = 0x00,
  };

  rc = lr1121_modem_system_set_reg_mode(ctx, LR1121_MODEM_SYSTEM_REG_MODE_DCDC);
  if (rc != RC_OK) {
    return rc;
  }
  /* TCXO supplied by the LR1121 (VTCXO pin) at 3.0 V. */
  rc = lr1121_modem_system_set_tcxo_mode(ctx, LR1121_MODEM_SYSTEM_TCXO_CTRL_3_0V, 255);
  if (rc != RC_OK) {
    return rc;
  }
  rc = lr1121_modem_system_set_dio_as_rf_switch(ctx, &sw);
  if (rc != RC_OK) {
    return rc;
  }
  rc = lr1121_modem_set_rf_output(ctx, LR1121_MODEM_BSP_RADIO_PA_SEL_LP);
  if (rc != RC_OK) {
    return rc;
  }
  rc = lr1121_modem_system_cfg_lfclk(ctx, LR1121_MODEM_SYSTEM_LFCLK_XTAL, true);
  if (rc != RC_OK) {
    return rc;
  }
  k_msleep(500); /* the 32 kHz crystal needs time to start */
  return lr1121_modem_set_tx_power_offset(ctx, CONFIG_LPWAN_SHIELD_LORA_TX_OFFSET_DB);
}

static bool drain_events(void);

static int ensure_started(void) {
  lr1121_modem_version_t v;
  lr1121_modem_response_code_t rc;

  if (started) {
    return 0;
  }
  if (lr1121_modem_hal_reset(ctx) != LR1121_MODEM_HAL_STATUS_OK) {
    return -EIO;
  }
  rc = lr1121_modem_get_modem_version(ctx, &v);
  if (rc != RC_OK) {
    LOG_ERR("LR1121 does not answer as Modem-E (rc %d): flash the Modem-E firmware", (int)rc);
    return -ENODEV;
  }
  snprintf(modem_str, sizeof(modem_str), "LR1121 ME %u.%u.%u", v.modem_major, v.modem_minor, v.modem_patch);
  LOG_INF("%s, LoRa Basics Modem %u.%u.%u", modem_str, v.lbm_major, v.lbm_minor, v.lbm_patch);
  drain_events(); /* the reset event */
  rc = configure_radio();
  if (rc != RC_OK) {
    LOG_ERR("radio setup failed (rc %d)", (int)rc);
    return -EIO;
  }
  started = true;
  state = L_IDLE;
  return 0;
}

static void wipe_keys(void) {
  memset(&keys, 0, sizeof(keys));
  keys_dirty = false;
}

/* Write the keys to the LR1121. They are wiped from RAM either way: after a failure the master has to
 * provision again (it dropped its copy when we accepted the request). */
static int program_keys(void) {
  lr1121_modem_dev_eui_t eui = {0};
  int ret = 0;

  if (lr1121_modem_set_dev_eui(ctx, keys.dev_eui) != RC_OK || lr1121_modem_set_join_eui(ctx, keys.join_eui) != RC_OK ||
      lr1121_modem_set_nwk_key(ctx, keys.app_key) != RC_OK) {
    ret = -EIO;
  } else if (lr1121_modem_get_dev_eui(ctx, eui) != RC_OK || memcmp(eui, keys.dev_eui, 8) != 0) {
    LOG_ERR("DevEUI verification failed");
    ret = -EIO;
  }
  wipe_keys();
  return ret;
}

static int apply_adr(void) {
  lr1121_modem_response_code_t rc;

  if (cfg.adr) {
    rc = lr1121_modem_set_adr_profile(ctx, LR1121_MODEM_ADR_PROFILE_NETWORK_SERVER_CONTROLLED, NULL);
  } else {
    uint8_t list[16];

    memset(list, cfg.data_rate, sizeof(list));
    rc = lr1121_modem_set_adr_profile(ctx, LR1121_MODEM_ADR_PROFILE_CUSTOM, list);
  }
  return rc == RC_OK ? 0 : -EIO;
}

static int apply_class(void) {
  lr1121_modem_classes_t c =
      cfg.lora_class == sensei_LoraClass_CLASS_C ? LR1121_LORAWAN_CLASS_C : LR1121_LORAWAN_CLASS_A;

  return lr1121_modem_set_class(ctx, c) == RC_OK ? 0 : -EIO;
}

/* Joined (new or restored): set ADR and class, then report ready. */
static void become_ready(void) {
  state = L_JOINED;
  if (apply_adr() || apply_class()) {
    state = L_IDLE;
    svc_emit_error(sensei_LpwanResult_LPWAN_FAILED, "ADR or class setup failed");
    return;
  }
  state = L_READY;
  svc_emit_radio(sensei_LpwanEvent_joined_tag, sensei_Radio_RADIO_LORA);
}

/* ---- commands ---- */

static void do_join(void) {
  lr1121_modem_lorawan_status_bitmask_t st = 0;

  join_pending = false;
  if (ensure_started()) {
    wipe_keys(); /* the master dropped its copy: provision again when the LR1121 is there */
    svc_emit_error(sensei_LpwanResult_LPWAN_FAILED, "LR1121 not available");
    return;
  }
  (void)lr1121_modem_get_status(ctx, &st);
  if ((st & LR1121_LORAWAN_JOINED) && !keys_dirty) {
    become_ready(); /* the session survived (power cycle or parked) */
    return;
  }
  if (st & (LR1121_LORAWAN_JOINED | LR1121_LORAWAN_JOINING)) {
    (void)lr1121_modem_leave_network(ctx);
  }
  if (keys_dirty) {
    if (program_keys()) {
      svc_emit_error(sensei_LpwanResult_LPWAN_FAILED, "key programming failed");
      return;
    }
  } else {
    lr1121_modem_dev_eui_t eui = {0};

    if (lr1121_modem_get_dev_eui(ctx, eui) != RC_OK || all_same(eui, 8, 0x00) || all_same(eui, 8, 0xFF)) {
      svc_emit_error(sensei_LpwanResult_LPWAN_NOT_CONFIGURED, "no LoRaWAN keys");
      return;
    }
  }
  if (lr1121_modem_set_region(ctx, region_to_lr(cfg.region)) != RC_OK) {
    svc_emit_error(sensei_LpwanResult_LPWAN_FAILED, "region");
    return;
  }
  join_failures = 0;
  state = L_JOINING;
  if (lr1121_modem_join(ctx) != RC_OK) {
    state = L_IDLE;
    svc_emit_error(sensei_LpwanResult_LPWAN_FAILED, "join request");
  }
}

static void do_send(const struct msg *m) {
  lr1121_modem_response_code_t rc;

  uint8_t max = 0;

  if (state != L_READY) {
    svc_emit_tx_done(m->seq, false, 0, 0);
    return;
  }
  /* The data rate and pending MAC commands decide what fits in the next uplink (the master chunks by
   * the configured data rate; with ADR the network may have lowered it). */
  if (lr1121_modem_get_next_tx_max_payload(ctx, &max) == RC_OK && m->len > max) {
    LOG_WRN("payload %u B does not fit the next uplink (%u B)", (unsigned)m->len, (unsigned)max);
    svc_emit_tx_done(m->seq, false, 0, max);
    return;
  }
  cur_seq = m->seq;
  cur_confirmed = m->confirmed;
  rc = lr1121_modem_request_tx(
      ctx, m->port, m->confirmed ? LR1121_MODEM_UPLINK_CONFIRMED : LR1121_MODEM_UPLINK_UNCONFIRMED, m->data, m->len);
  if (rc != RC_OK) {
    LOG_WRN("request_tx failed (rc %d)", (int)rc);
    svc_emit_tx_done(m->seq, false, 0, 0);
    return;
  }
  state = L_SENDING;
}

/* The master let go while a join was queued or running: stop it, so a late "joined" does not connect a
 * radio nobody wants. In order with the join, on this thread (the LR1121 is only used here). */
static void do_leave(void) {
  if (started && state == L_JOINING) {
    (void)lr1121_modem_leave_network(ctx);
  }
  if (state == L_JOINING || state == L_JOINED || state == L_READY) {
    state = L_IDLE;
  }
  svc_emit_radio(sensei_LpwanEvent_disconnected_tag, sensei_Radio_RADIO_LORA);
}

static void do_time(void) {
  if (state != L_READY) {
    return;
  }
  /* DeviceTimeReq rides on the next uplink: send an empty one now. */
  if (lr1121_modem_mac_request_tx(ctx, LR1121_MODEM_MAC_REQUEST_TIME) == RC_OK) {
    (void)lr1121_modem_request_empty_tx(ctx, false, 0, LR1121_MODEM_UPLINK_UNCONFIRMED);
  }
}

/* ---- events from the LR1121 ---- */

static void handle_downlink(void) {
  static uint8_t buf[255];
  lr1121_modem_downlink_metadata_t meta;
  uint8_t size = 0, remaining = 0;

  do {
    if (lr1121_modem_get_downlink_data_size(ctx, &size, &remaining) != RC_OK || size == 0) {
      return;
    }
    if (lr1121_modem_get_downlink_data(ctx, buf, size) != RC_OK ||
        lr1121_modem_get_downlink_metadata(ctx, &meta) != RC_OK) {
      return;
    }
    last_rssi = meta.rssi;
    last_snr = meta.snr_integer;
    svc_emit_downlink(sensei_Radio_RADIO_LORA, meta.fport, buf, size);
  } while (remaining > 0);
}

static void handle_event(const lr1121_modem_event_fields_t *e) {
  uint8_t status = (uint8_t)(e->data >> 8);

  LOG_DBG("modem event 0x%02x data 0x%04x", (unsigned)e->event_type, (unsigned)e->data);
  switch (e->event_type) {
  case LR1121_MODEM_LORAWAN_EVENT_RESET:
    if (state != L_OFF && started) {
      enum lstate was = state;

      LOG_WRN("LR1121 reset by itself");
      /* The radio setup is gone: ensure_started() runs it again at the next join. Tell the master. */
      started = false;
      state = L_IDLE;
      if (was == L_SENDING) {
        svc_emit_tx_done(cur_seq, false, 0, 0);
      }
      if (was != L_IDLE) {
        svc_emit_radio(sensei_LpwanEvent_disconnected_tag, sensei_Radio_RADIO_LORA);
      }
    }
    break;
  case LR1121_MODEM_LORAWAN_EVENT_JOINED:
    if (state == L_JOINING) { /* not after a leave: the master gave up this join */
      become_ready();
    }
    break;
  case LR1121_MODEM_LORAWAN_EVENT_JOIN_FAIL:
    if (state == L_JOINING && ++join_failures >= CONFIG_LPWAN_SHIELD_LORA_JOIN_ATTEMPTS) {
      (void)lr1121_modem_leave_network(ctx);
      state = L_IDLE;
      svc_emit_error(sensei_LpwanResult_LPWAN_NOT_JOINED, "join failed");
    }
    break;
  case LR1121_MODEM_LORAWAN_EVENT_TX_DONE: {
    bool ok = cur_confirmed ? status == TX_CONFIRMED : status != TX_NOT_SENT;
    uint8_t next_max = 0;

    if (state != L_SENDING) {
      break; /* the empty uplink of a time request, or a stray event */
    }
    state = L_READY;
    /* The MAC answers the receive windows brought are known now: what the next uplink takes. */
    if (lr1121_modem_get_next_tx_max_payload(ctx, &next_max) != RC_OK) {
      next_max = 0;
    }
    svc_emit_tx_done(cur_seq, ok, 1, next_max);
    break;
  }
  case LR1121_MODEM_LORAWAN_EVENT_DOWN_DATA:
    handle_downlink();
    break;
  case LR1121_MODEM_LORAWAN_EVENT_LORAWAN_MAC_TIME: {
    uint32_t gps_s = 0, frac = 0;

    if (status != 0 && lr1121_modem_get_lorawan_mac_time(ctx, &gps_s, &frac) == RC_OK) {
      svc_emit_time(sensei_Radio_RADIO_LORA, gps_s + GPS_TO_UNIX_S - GPS_UTC_LEAP_S);
    }
    break;
  }
  case LR1121_MODEM_LORAWAN_EVENT_REGIONAL_DUTY_CYCLE:
    if (status == 0x00) {
      LOG_WRN("duty cycle: transmissions blocked");
    }
    break;
  default:
    break;
  }
}

/* Read events until the LR1121 has none (or 16 in one go). Returns true if it stopped at 16: there may be more. */
static bool drain_events(void) {
  for (int i = 0; i < 16; i++) {
    lr1121_modem_event_fields_t e = {0};
    lr1121_modem_response_code_t rc = lr1121_modem_get_event(ctx, &e);

    if (rc == LR1121_MODEM_RESPONSE_CODE_NO_EVENT) {
      return false;
    }
    if (rc != RC_OK) {
      LOG_WRN("get_event failed (rc %d)", (int)rc);
      return false;
    }
    handle_event(&e);
  }
  return true;
}

/* The event line (DIO9) is edge-triggered here: an edge that arrives while the queue is full, or events left
 * after a drain, would be missed for good. So the ISR also sets a flag, and the worker reads again
 * after every command until the event line is low. */
static atomic_t ev_missed;

static void event_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
  static const struct msg m = {.cmd = CMD_EVENT};

  ARG_UNUSED(port);
  ARG_UNUSED(cb);
  ARG_UNUSED(pins);
  atomic_set(&ev_missed, 1);
  (void)k_msgq_put(&lora_q, &m, K_NO_WAIT);
}

static void service_events(void) {
  const struct gpio_dt_spec *ev = &lr1121_ctx.spi_gpio.event;

  while (started && (atomic_cas(&ev_missed, 1, 0) || gpio_pin_get_dt(ev) > 0)) {
    if (!drain_events() && gpio_pin_get_dt(ev) > 0) {
      /* The line is still high but there is nothing to read: do not spin. */
      break;
    }
  }
}

static void lora_main(void *a, void *b, void *c) {
  static struct msg m;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    k_msgq_get(&lora_q, &m, K_FOREVER);
    switch (m.cmd) {
    case CMD_EVENT:
      break; /* service_events() below */
    case CMD_JOIN:
      do_join();
      break;
    case CMD_SEND:
      do_send(&m);
      break;
    case CMD_TIME:
      do_time();
      break;
    case CMD_LEAVE:
      do_leave();
      break;
    }
    service_events();
  }
}

/* ---- API ---- */

int lora_init(void) {
  const struct gpio_dt_spec *ev = &lr1121_ctx.spi_gpio.event;
  int err = lr1121_hal_init();

  if (err) {
    return err;
  }
  gpio_init_callback(&ev_cb, event_isr, BIT(ev->pin));
  err = gpio_add_callback(ev->port, &ev_cb);
  err |= gpio_pin_interrupt_configure_dt(ev, GPIO_INT_EDGE_TO_ACTIVE);
  if (err) {
    return -EIO;
  }
  k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), lora_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_WORKER_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&thread, "lora");
  return 0;
}

static sensei_LpwanResult post(const struct msg *m) {
  return k_msgq_put(&lora_q, m, K_NO_WAIT) == 0 ? sensei_LpwanResult_LPWAN_OK : sensei_LpwanResult_LPWAN_BUSY;
}

sensei_LpwanResult lora_configure(const sensei_LoraConfigure *c) {
  if (c->lora_class == sensei_LoraClass_CLASS_B) {
    return sensei_LpwanResult_LPWAN_UNSUPPORTED;
  }
  if (c->data_rate > 15 || c->port == 0 || c->port > 223) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  cfg = *c;
  if (started && state == L_READY) {
    (void)apply_adr(); /* takes effect at once; the class is set at the next join */
  }
  return sensei_LpwanResult_LPWAN_OK;
}

sensei_LpwanResult lora_provision(const sensei_LoraProvision *k) {
  keys = *k; /* kept in RAM only until they are written to the LR1121 at the next join */
  keys_dirty = true;
  return sensei_LpwanResult_LPWAN_OK;
}

sensei_LpwanResult lora_join(void) {
  const struct msg m = {.cmd = CMD_JOIN};
  sensei_LpwanResult r;

  join_pending = true;
  r = post(&m);
  if (r != sensei_LpwanResult_LPWAN_OK) {
    join_pending = false;
  }
  return r;
}

sensei_LpwanResult lora_send(uint32_t seq, uint32_t port, bool confirmed, const uint8_t *data, size_t len,
                             uint32_t *expected_ms, uint32_t *next_max) {
  struct msg m = {.cmd = CMD_SEND, .seq = seq, .confirmed = confirmed};
  uint8_t max = 0;
  int32_t duty = 0;
  uint32_t wait_ms = 0;

  *expected_ms = 0;
  *next_max = 0;
  if (len == 0 || len > sizeof(m.data)) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  if (state != L_READY) {
    /* SENDING: a frame still sits in the LR1121 (a send the watchdog gave up). It cannot be taken back. */
    return state == L_SENDING ? sensei_LpwanResult_LPWAN_BUSY : sensei_LpwanResult_LPWAN_NOT_JOINED;
  }
  /* Read-only questions to the LR1121 (each is one locked transaction, the worker may be busy elsewhere). */
  if (lr1121_modem_get_next_tx_max_payload(ctx, &max) == RC_OK) {
    *next_max = max;
    if (len > max) {
      LOG_WRN("payload %u B does not fit the next uplink (%u B)", (unsigned)len, (unsigned)max);
      return sensei_LpwanResult_LPWAN_INVALID; /* the master chunks again */
    }
  }
  /* The LR1121 holds a frame back until the duty cycle allows it (1 %: minutes after a full chunk at DR0). */
  if (lr1121_modem_get_duty_cycle_status(ctx, &duty) == RC_OK && duty < 0) {
    wait_ms = (uint32_t)(-(int64_t)duty);
  }
  *expected_ms = lpwan_lora_expected_ms(cfg.data_rate, cfg.adr, (uint32_t)len, wait_ms);
  if (wait_ms > 0) {
    LOG_INF("duty cycle: send %u waits %u ms, result expected in %u ms", (unsigned)seq, (unsigned)wait_ms,
            (unsigned)*expected_ms);
  }
  m.port = (uint8_t)(port ? port : cfg.port);
  m.len = (uint8_t)len;
  memcpy(m.data, data, len);
  return post(&m);
}

sensei_LpwanResult lora_request_time(void) {
  const struct msg m = {.cmd = CMD_TIME};

  return post(&m);
}

sensei_LpwanResult lora_leave(void) {
  const struct msg m = {.cmd = CMD_LEAVE};

  /* A join is queued or running: the worker stops it and reports `disconnected` after it. */
  if (state == L_JOINING || join_pending) {
    return post(&m);
  }
  /* Park: the session stays in the LR1121, the next join restores it. */
  if (state == L_READY || state == L_JOINED) {
    state = L_IDLE;
  }
  svc_emit_radio(sensei_LpwanEvent_disconnected_tag, sensei_Radio_RADIO_LORA);
  return sensei_LpwanResult_LPWAN_OK;
}

sensei_LpwanResult lora_sleep(uint32_t deep) {
  ARG_UNUSED(deep); /* the Modem-E firmware sleeps by itself between events */
  return sensei_LpwanResult_LPWAN_OK;
}

void lora_fill_status(sensei_LpwanStatus *st) {
  st->lora_joined = state == L_READY || state == L_SENDING;
  if (st->lora_joined) {
    st->rssi_dbm = last_rssi;
    st->snr_db = last_snr;
  }
}

void lora_modem_string(char *out, size_t cap) { snprintf(out, cap, "%s", modem_str); }
