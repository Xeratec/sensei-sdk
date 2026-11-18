/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * The LR1121 with the Modem-E firmware for native_sim: a fake of the driver interface that lora.c calls (the
 * lr1121_modem_* functions of the vendored Semtech driver, whose headers it uses), instead of the driver and the SPI
 * HAL. The SPI commands of the driver are not exercised.
 *
 * What it models, as the Modem-E firmware does:
 *  - the LoRaWAN session lives in the "flash" of the radio (-lr_state file): it survives a restart of the shield;
 *  - OTAA: a join request goes to the network server (the harness, over the control connection), the answer decides
 *    between JOINED and JOIN_FAIL (retried every 10 s);
 *  - an uplink waits for the duty cycle of EU868 (1 %: the next frame is held back until 100 times the airtime after
 *    the start of the last one; the host test model of lib/sensei_lpwan/tests/test_lpwan_lora.c), is sent NbTrans times
 *    (and once more if it is confirmed and not acknowledged), each followed by the two receive windows; TX_DONE comes
 *    after the last. The network server sees the first frame only (the repeats have the same counter);
 *  - the downlink the server answers with is delivered before TX_DONE;
 *  - events with the EVENT line (DIO9, an emulated GPIO pin): RESET, JOINED, JOIN_FAIL, TX_DONE, DOWN_DATA, MAC_TIME.
 * Not modelled: radio parameters, ADR changes by the network, class B and C, LBT, relay, FUOTA.
 *
 * Commands of the harness ("lr ..."): see README.md.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lr1121_modem_bsp.h"
#include "lr1121_modem_lorawan.h"
#include "lr1121_modem_modem.h"
#include "lr1121_modem_system.h"
#include "lr1121_zephyr_hal.h"
#include "sensei_lpwan/lpwan_lora.h"
#include "sensei_lpwan/lpwan_sim.h"
#include "sim_shield.h"

LOG_MODULE_REGISTER(sim_lr1121, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define RC_OK LR1121_MODEM_RESPONSE_CODE_OK
#define LR_NODE DT_NODELABEL(sim_lr1121)

#define RX_MS 2100u      /* the two receive windows after a frame, as in the host test model */
#define JOIN_RX_MS 5000u /* the join accept comes in RX1, 5 s after the request */
#define JOIN_RETRY_MS 10000u
#define VERDICT_WAIT_MS 20000u /* longest wait for the answer of the network server */
#define GPS_TO_UNIX_S 315964800u
#define GPS_UTC_LEAP_S 18u
#define NV_MAGIC 0x4C523131u

struct lr1121_context lr1121_ctx = {
    .spi_gpio = {.event = GPIO_DT_SPEC_GET(LR_NODE, event_gpios)},
    .status = {.radio_status = RADIO_AWAKE},
};

/* What the radio keeps in its flash. */
static struct nv {
  uint32_t magic;
  uint8_t dev_eui[8], join_eui[8], nwk_key[16];
  uint8_t joined;
  uint8_t region;
  uint32_t fcnt;
} nv;

struct event {
  uint8_t type;
  uint16_t data;
};

struct downlink {
  uint8_t fport, size;
  uint8_t data[255];
};

static K_MUTEX_DEFINE(lock);
static struct k_work_delayable work; /* the next step of a join or a send */
static bool up;                      /* started (reset done) */
static bool joining;
static bool adr;
static uint8_t dr = 5;
static uint8_t nb_trans = 1;
static uint8_t max_override;
static unsigned slow_pct = 100;
static bool hang;
static bool time_requested;
static struct event evq[16];
static unsigned ev_n;
static struct downlink dlq[4];
static unsigned dl_n;
static uint32_t blocked_until; /* uptime: the duty cycle holds the next frame back until then */

enum step { STEP_NONE, STEP_JOIN_SEND, STEP_JOIN_WAIT, STEP_TX_SEND, STEP_TX_WAIT, STEP_TX_DONE };

static struct {
  enum step step;
  /* join */
  bool verdict; /* the network server answered */
  bool accept;
  uint32_t asked_at;
  /* uplink */
  bool active, flush, confirmed;
  uint8_t port, len, data[222];
  uint32_t air_ms, start_ms;
  bool ack, has_down;
  struct downlink down;
} t;

static void save(void) {
  const char *path = sim_opt_lr_state();

  if (path != NULL && lpwan_sim_file_write(path, &nv, sizeof(nv)) != 0) {
    LOG_WRN("LR1121 state not saved");
  }
}

static void load(void) {
  const char *path = sim_opt_lr_state();
  struct nv in;

  if (path != NULL && lpwan_sim_file_read(path, &in, sizeof(in)) == (long)sizeof(in) && in.magic == NV_MAGIC) {
    nv = in;
    LOG_INF("LR1121: session restored from %s (%s)", path, nv.joined ? "joined" : "not joined");
  } else {
    memset(&nv, 0, sizeof(nv));
    nv.magic = NV_MAGIC;
    nv.region = LR1121_LORAWAN_REGION_EU868;
  }
}

static int32_t until(uint32_t at) { return (int32_t)(at - k_uptime_get_32()); }

/* The EVENT line (DIO9): high while an event waits. Under lock; the callback of lora.c takes no lock. */
static void set_line(bool level) {
  const struct gpio_dt_spec *ev = &lr1121_ctx.spi_gpio.event;

  (void)gpio_emul_input_set(ev->port, ev->pin, level ? 1 : 0);
}

static void post(uint8_t type, uint16_t data) {
  if (ev_n < ARRAY_SIZE(evq)) {
    evq[ev_n++] = (struct event){type, data};
    set_line(true);
  }
}

static void schedule(enum step s, int32_t ms) {
  t.step = s;
  (void)k_work_reschedule(&work, K_MSEC(ms > 0 ? ms : 0));
}

static void cancel_all(void) {
  t.step = STEP_NONE;
  t.active = false;
  joining = false;
  (void)k_work_cancel_delayable(&work);
}

/* ---- the steps ---- */

static void hex_line(char *out, const uint8_t *p, int n) { sim_hex_encode(p, n, out); }

static void join_send(void) {
  char deveui[17], joineui[17], key[33];

  hex_line(deveui, nv.dev_eui, 8);
  hex_line(joineui, nv.join_eui, 8);
  hex_line(key, nv.nwk_key, 16);
  t.verdict = false;
  t.asked_at = k_uptime_get_32();
  if (!sim_ctl_send("join %s %s %s %u", deveui, joineui, key, (unsigned)nv.region)) {
    LOG_WRN("join: no network server connected");
  }
  schedule(STEP_JOIN_WAIT, JOIN_RX_MS);
}

static void join_wait(void) {
  if (t.verdict && t.accept) {
    nv.joined = 1;
    nv.fcnt = 0;
    save();
    joining = false;
    t.step = STEP_NONE;
    LOG_INF("LR1121: joined");
    post(LR1121_MODEM_LORAWAN_EVENT_JOINED, 0);
    return;
  }
  if (!t.verdict && (uint32_t)(k_uptime_get_32() - t.asked_at) < VERDICT_WAIT_MS && sim_ctl_connected()) {
    schedule(STEP_JOIN_WAIT, 10); /* the answer is on its way (the harness runs in real time) */
    return;
  }
  LOG_WRN("LR1121: join failed (%s)", t.verdict ? "rejected" : "no answer");
  post(LR1121_MODEM_LORAWAN_EVENT_JOIN_FAIL, 0);
  schedule(STEP_JOIN_SEND, JOIN_RETRY_MS);
}

static uint32_t air_ms(uint8_t len) { return lpwan_lora_airtime_ms(dr, (uint32_t)len + 13u); }

/* The first frame goes out now: the network server sees it. */
static void tx_send(void) {
  char hex[2 * sizeof(t.data) + 2];

  t.verdict = false;
  t.ack = false;
  t.has_down = false;
  t.asked_at = k_uptime_get_32();
  sim_hex_encode(t.data, t.len, hex);
  if (!sim_ctl_send("up %u %u %u %u %u %s", (unsigned)nv.fcnt, (unsigned)t.port, t.confirmed ? 1u : 0u, (unsigned)dr,
                    (unsigned)k_uptime_get_32(), t.len ? hex : "-")) {
    t.verdict = true; /* nobody there: no answer, no downlink */
  }
  schedule(STEP_TX_WAIT, (int32_t)((t.air_ms + RX_MS) * slow_pct / 100u));
}

static void tx_wait(void) {
  uint32_t n_total, last_start, done;

  if (!t.verdict && (uint32_t)(k_uptime_get_32() - t.asked_at) < VERDICT_WAIT_MS) {
    schedule(STEP_TX_WAIT, 10);
    return;
  }
  if (t.has_down && dl_n < ARRAY_SIZE(dlq)) {
    dlq[dl_n++] = t.down;
    post(LR1121_MODEM_LORAWAN_EVENT_DOWN_DATA, 0);
  }
  /* NbTrans frames; a confirmed uplink that was not acknowledged goes once more. The first one is out already. */
  n_total = t.confirmed ? (t.ack ? 1u : nb_trans + 1u) : nb_trans;
  last_start = t.start_ms;
  for (uint32_t k = 1; k < n_total; k++) {
    uint32_t after_rx = last_start + t.air_ms + RX_MS;

    last_start = (int32_t)(after_rx - blocked_until) > 0 ? after_rx : blocked_until;
    blocked_until = last_start + 100u * t.air_ms;
  }
  done = last_start + t.air_ms + RX_MS;
  nv.fcnt++;
  save();
  t.start_ms = (uint32_t)n_total; /* kept for TX_DONE */
  if (hang) {
    t.step = STEP_NONE; /* the Modem-E firmware hangs: no TX_DONE */
    return;
  }
  schedule(STEP_TX_DONE, until(done) * (int32_t)slow_pct / 100);
}

static void tx_done(void) {
  uint8_t status = t.confirmed ? (t.ack ? 2 : 0) : 1;

  t.step = STEP_NONE;
  t.active = false;
  post(LR1121_MODEM_LORAWAN_EVENT_TX_DONE, (uint16_t)(status << 8 | (uint8_t)t.start_ms));
  if (time_requested) {
    time_requested = false;
    post(LR1121_MODEM_LORAWAN_EVENT_LORAWAN_MAC_TIME, 0x0100);
  }
}

static void work_fn(struct k_work *w) {
  ARG_UNUSED(w);
  k_mutex_lock(&lock, K_FOREVER);
  switch (t.step) {
  case STEP_JOIN_SEND:
    join_send();
    break;
  case STEP_JOIN_WAIT:
    join_wait();
    break;
  case STEP_TX_SEND:
    tx_send();
    break;
  case STEP_TX_WAIT:
    tx_wait();
    break;
  case STEP_TX_DONE:
    tx_done();
    break;
  default:
    break;
  }
  k_mutex_unlock(&lock);
}

/* ---- the hardware layer of lora.c ---- */

int lr1121_hal_init(void) {
  const struct gpio_dt_spec *ev = &lr1121_ctx.spi_gpio.event;

  if (!gpio_is_ready_dt(ev)) {
    return -ENODEV;
  }
  return gpio_pin_configure_dt(ev, GPIO_INPUT) ? -EIO : 0;
}

/* The flash of the radio is there before the harness can talk to the shield. */
static int lr_boot(void) {
  load();
  k_work_init_delayable(&work, work_fn);
  return 0;
}
SYS_INIT(lr_boot, APPLICATION, 90);

lr1121_modem_hal_status_t lr1121_modem_hal_reset(const void *context) {
  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  cancel_all();
  ev_n = 0;
  dl_n = 0;
  time_requested = false;
  blocked_until = 0; /* the duty-cycle memory is in RAM of the radio */
  up = true;
  post(LR1121_MODEM_LORAWAN_EVENT_RESET, 0);
  k_mutex_unlock(&lock);
  k_msleep(1000); /* the firmware needs about a second to start */
  return LR1121_MODEM_HAL_STATUS_OK;
}

/* ---- driver calls ---- */

lr1121_modem_response_code_t lr1121_modem_get_modem_version(const void *context, lr1121_modem_version_t *v) {
  ARG_UNUSED(context);
  *v = (lr1121_modem_version_t){.use_case = 5,
                                .modem_major = 2,
                                .modem_minor = 0,
                                .modem_patch = 2,
                                .lbm_major = 4,
                                .lbm_minor = 8,
                                .lbm_patch = 1};
  return up ? RC_OK : LR1121_MODEM_RESPONSE_CODE_NOT_INITIALIZED;
}

lr1121_modem_response_code_t lr1121_modem_get_event(const void *context, lr1121_modem_event_fields_t *e) {
  lr1121_modem_response_code_t rc = LR1121_MODEM_RESPONSE_CODE_NO_EVENT;

  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  if (ev_n > 0) {
    e->event_type = (lr1121_modem_lorawan_event_type_t)evq[0].type;
    e->missed_events_count = 0;
    e->data = evq[0].data;
    memmove(&evq[0], &evq[1], (ev_n - 1) * sizeof(evq[0]));
    ev_n--;
    rc = RC_OK;
  }
  if (ev_n == 0) {
    set_line(false);
  }
  k_mutex_unlock(&lock);
  return rc;
}

lr1121_modem_response_code_t lr1121_modem_get_status(const void *context, lr1121_modem_lorawan_status_bitmask_t *st) {
  ARG_UNUSED(context);
  *st = (uint8_t)((nv.joined ? LR1121_LORAWAN_JOINED : 0) | (joining ? LR1121_LORAWAN_JOINING : 0));
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_dev_eui(const void *context, lr1121_modem_dev_eui_t eui) {
  ARG_UNUSED(context);
  memcpy(eui, nv.dev_eui, 8);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_dev_eui(const void *context, const lr1121_modem_dev_eui_t eui) {
  ARG_UNUSED(context);
  memcpy(nv.dev_eui, eui, 8);
  save();
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_join_eui(const void *context, const lr1121_modem_join_eui_t eui) {
  ARG_UNUSED(context);
  memcpy(nv.join_eui, eui, 8);
  save();
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_nwk_key(const void *context, const lr1121_modem_nwk_key_t key) {
  ARG_UNUSED(context);
  memcpy(nv.nwk_key, key, 16);
  save();
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_region(const void *context, const lr1121_modem_regions_t region) {
  ARG_UNUSED(context);
  nv.region = (uint8_t)region;
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_class(const void *context, const lr1121_modem_classes_t c) {
  ARG_UNUSED(context);
  ARG_UNUSED(c);
  return RC_OK;
}

lr1121_modem_response_code_t
lr1121_modem_set_adr_profile(const void *context, const lr1121_modem_adr_profiles_t profile, const uint8_t *list) {
  ARG_UNUSED(context);
  adr = profile == LR1121_MODEM_ADR_PROFILE_NETWORK_SERVER_CONTROLLED;
  if (!adr && list != NULL) {
    dr = list[0];
  }
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_join(const void *context) {
  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  joining = true;
  schedule(STEP_JOIN_SEND, 0);
  k_mutex_unlock(&lock);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_leave_network(const void *context) {
  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  if (t.step == STEP_JOIN_SEND || t.step == STEP_JOIN_WAIT) {
    t.step = STEP_NONE;
    (void)k_work_cancel_delayable(&work);
  }
  joining = false;
  nv.joined = 0;
  save();
  k_mutex_unlock(&lock);
  return RC_OK;
}

static uint8_t next_max(void) { return max_override != 0 ? max_override : (uint8_t)lpwan_lora_max_payload(dr, false); }

static lr1121_modem_response_code_t start_tx(uint8_t port, bool confirmed, const uint8_t *data, uint8_t len,
                                             bool flush) {
  lr1121_modem_response_code_t rc = RC_OK;
  uint32_t now = k_uptime_get_32();
  uint32_t start;

  k_mutex_lock(&lock, K_FOREVER);
  if (!nv.joined) {
    rc = LR1121_MODEM_RESPONSE_CODE_NO_TIME;
  } else if (t.active) {
    rc = LR1121_MODEM_RESPONSE_CODE_BUSY;
  } else if (len > next_max()) {
    rc = LR1121_MODEM_RESPONSE_CODE_INVALID;
  } else {
    t.active = true;
    t.flush = flush;
    t.confirmed = confirmed;
    t.port = port;
    t.len = len;
    if (len > 0) {
      memcpy(t.data, data, len);
    }
    t.air_ms = air_ms(len);
    start = (int32_t)(now - blocked_until) > 0 ? now : blocked_until; /* the duty cycle holds it back */
    t.start_ms = start;
    blocked_until = start + 100u * t.air_ms;
    schedule(STEP_TX_SEND, until(start));
    LOG_INF("LR1121: uplink of %u B queued, goes out in %d ms", len, (int)until(start));
  }
  k_mutex_unlock(&lock);
  return rc;
}

lr1121_modem_response_code_t lr1121_modem_request_tx(const void *context, const uint8_t port,
                                                     const lr1121_modem_uplink_type_t type, const uint8_t *data,
                                                     const uint8_t length) {
  ARG_UNUSED(context);
  return start_tx(port, type == LR1121_MODEM_UPLINK_CONFIRMED, data, length, false);
}

lr1121_modem_response_code_t lr1121_modem_request_empty_tx(const void *context, bool is_fport_populated,
                                                           const uint8_t port, const lr1121_modem_uplink_type_t type) {
  ARG_UNUSED(context);
  return start_tx(is_fport_populated ? port : 0, type == LR1121_MODEM_UPLINK_CONFIRMED, NULL, 0, true);
}

lr1121_modem_response_code_t lr1121_modem_mac_request_tx(const void *context,
                                                         const lr1121_modem_mac_request_bitmask_t req) {
  ARG_UNUSED(context);
  time_requested = (req & LR1121_MODEM_MAC_REQUEST_TIME) != 0;
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_lorawan_mac_time(const void *context, uint32_t *gps_s, uint32_t *frac) {
  int64_t utc_ms = lpwan_sim_utc_ms();

  ARG_UNUSED(context);
  *gps_s = (uint32_t)(utc_ms / 1000) - GPS_TO_UNIX_S + GPS_UTC_LEAP_S;
  *frac = 0;
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_next_tx_max_payload(const void *context, uint8_t *max) {
  ARG_UNUSED(context);
  *max = next_max();
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_duty_cycle_status(const void *context, int32_t *duty) {
  int32_t left;

  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  left = until(blocked_until);
  k_mutex_unlock(&lock);
  *duty = left > 0 ? -left : 0; /* negative: the radio holds a frame back this long */
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_nb_trans(const void *context, uint8_t *n) {
  ARG_UNUSED(context);
  *n = nb_trans;
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_downlink_data_size(const void *context, uint8_t *size,
                                                                 uint8_t *remaining) {
  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  *size = dl_n > 0 ? dlq[0].size : 0;
  *remaining = dl_n > 0 ? (uint8_t)(dl_n - 1) : 0;
  k_mutex_unlock(&lock);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_get_downlink_data(const void *context, uint8_t *buf, uint8_t size) {
  ARG_UNUSED(context);
  k_mutex_lock(&lock, K_FOREVER);
  if (dl_n > 0) {
    memcpy(buf, dlq[0].data, MIN(size, dlq[0].size));
  }
  k_mutex_unlock(&lock);
  return RC_OK;
}

/* The last call of the three that read a downlink: it leaves the queue. */
lr1121_modem_response_code_t lr1121_modem_get_downlink_metadata(const void *context,
                                                                lr1121_modem_downlink_metadata_t *m) {
  ARG_UNUSED(context);
  memset(m, 0, sizeof(*m));
  m->rssi = -70;
  m->snr_integer = 7;
  m->datarate = dr;
  m->frequency_hz = 869525000;
  k_mutex_lock(&lock, K_FOREVER);
  if (dl_n > 0) {
    m->fport = dlq[0].fport;
    memmove(&dlq[0], &dlq[1], (dl_n - 1) * sizeof(dlq[0]));
    dl_n--;
  }
  k_mutex_unlock(&lock);
  return RC_OK;
}

/* Settings of the radio front end: accepted. */
lr1121_modem_response_code_t lr1121_modem_set_rf_output(const void *context,
                                                        const lr1121_modem_bsp_radio_pa_selection_t o) {
  ARG_UNUSED(context);
  ARG_UNUSED(o);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_set_tx_power_offset(const void *context, const int8_t offset) {
  ARG_UNUSED(context);
  ARG_UNUSED(offset);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_system_cfg_lfclk(const void *context, const lr1121_modem_system_lfclk_cfg_t c,
                                                           const bool wait) {
  ARG_UNUSED(context);
  ARG_UNUSED(c);
  ARG_UNUSED(wait);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_system_set_dio_as_rf_switch(const void *context,
                                                                      const lr1121_modem_system_rfswitch_cfg_t *c) {
  ARG_UNUSED(context);
  ARG_UNUSED(c);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_system_set_reg_mode(const void *context,
                                                              const lr1121_modem_system_reg_mode_t m) {
  ARG_UNUSED(context);
  ARG_UNUSED(m);
  return RC_OK;
}

lr1121_modem_response_code_t lr1121_modem_system_set_tcxo_mode(const void *context,
                                                               const lr1121_modem_system_tcxo_supply_voltage_t tune,
                                                               const uint32_t timeout) {
  ARG_UNUSED(context);
  ARG_UNUSED(tune);
  ARG_UNUSED(timeout);
  return RC_OK;
}

/* ---- commands of the harness ---- */

void sim_lr_command(int argc, char **argv) {
  uint8_t buf[255];

  k_mutex_lock(&lock, K_FOREVER);
  if (argc >= 2 && strcmp(argv[0], "join") == 0) {
    t.verdict = true;
    t.accept = strcmp(argv[1], "accept") == 0;
  } else if (argc >= 4 && strcmp(argv[0], "keys") == 0) {
    /* the radio was provisioned at the factory: DevEUI, JoinEUI and AppKey in its flash (the node cannot give them) */
    uint8_t k[16];

    if (sim_hex_decode(argv[1], k, 8) == 8) {
      memcpy(nv.dev_eui, k, 8);
    }
    if (sim_hex_decode(argv[2], k, 8) == 8) {
      memcpy(nv.join_eui, k, 8);
    }
    if (sim_hex_decode(argv[3], k, 16) == 16) {
      memcpy(nv.nwk_key, k, 16);
    }
    save();
  } else if (argc >= 4 && strcmp(argv[0], "rx") == 0) {
    int n = sim_hex_decode(argv[3], buf, sizeof(buf));

    t.ack = argv[1][0] == '1';
    t.has_down = n > 0;
    if (n > 0) {
      t.down.fport = (uint8_t)atoi(argv[2]);
      t.down.size = (uint8_t)n;
      memcpy(t.down.data, buf, (size_t)n);
    }
    t.verdict = true;
  } else if (argc >= 3 && strcmp(argv[0], "down") == 0) {
    int n = sim_hex_decode(argv[2], buf, sizeof(buf));

    if (n > 0 && dl_n < ARRAY_SIZE(dlq)) {
      dlq[dl_n].fport = (uint8_t)atoi(argv[1]);
      dlq[dl_n].size = (uint8_t)n;
      memcpy(dlq[dl_n].data, buf, (size_t)n);
      dl_n++;
      post(LR1121_MODEM_LORAWAN_EVENT_DOWN_DATA, 0);
    }
  } else if (argc >= 1 && strcmp(argv[0], "reset") == 0) {
    /* the radio resets by itself (brown-out, firmware fault): volatile state and the frame in flight are gone */
    LOG_WRN("LR1121: reset by itself (injected)");
    cancel_all();
    ev_n = 0;
    dl_n = 0;
    blocked_until = 0;
    post(LR1121_MODEM_LORAWAN_EVENT_RESET, 0);
  } else if (argc >= 2 && strcmp(argv[0], "maxpayload") == 0) {
    max_override = (uint8_t)atoi(argv[1]); /* the MAC answers it owes use room: 0 = by the data rate */
  } else if (argc >= 2 && strcmp(argv[0], "nbtrans") == 0) {
    nb_trans = (uint8_t)CLAMP(atoi(argv[1]), 1, LPWAN_LORA_NB_TRANS_MAX);
  } else if (argc >= 2 && strcmp(argv[0], "dr") == 0) {
    dr = (uint8_t)atoi(argv[1]);
  } else if (argc >= 2 && strcmp(argv[0], "slow") == 0) {
    slow_pct = (unsigned)CLAMP(atoi(argv[1]), 10, 1000);
  } else if (argc >= 2 && strcmp(argv[0], "hang") == 0) {
    hang = argv[1][0] != '0';
  } else {
    LOG_WRN("unknown lr command");
  }
  k_mutex_unlock(&lock);
}
