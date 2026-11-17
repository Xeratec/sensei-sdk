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
 * @file lpwan_client.c
 * @brief Client of the LPWAN shield's nRF9151: shield power, shared SPI bus, the link thread,
 *        the radios (LoRa, LTE-M, NB-IoT), downlinks and network time.
 *
 * One thread owns the link (SPI, expander, state). Other contexts (the app, the shell) post
 * commands to it. Results go back through the hooks on the hook work queue: send completions,
 * downlinks, network time. See sensei_lpwan/lpwan_link.h for the transfer scheme.
 *
 * Bring-up, as the commissioning project and the thesis describe it: expander outputs first
 * (shield 3.3 V, buck-boost mode), then the level shifter enable, then SPI.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include "lpwan_priv.h"
#include "sensei_busarb/bus_arb.h"
#include "sensei_lpwan/lpwan.h"
#include "sensei_lpwan/lpwan_master.h"

LOG_MODULE_REGISTER(sensei_lpwan, CONFIG_SENSEI_LPWAN_LOG_LEVEL);

#define LINK_NODE DT_ALIAS(lpwan_link)
#define SPI_NODE DT_ALIAS(lpwan_spi)
#define SPI_BUS DT_BUS(SPI_NODE)

#define LORA_MTU SENSEI_LPWAN_MTU_LORA
#define LTE_MTU SENSEI_LPWAN_MTU_LTE

/* ---- app side: hooks and configuration ---- */

static struct sensei_lpwan_hooks hooks;
static struct sensei_lpwan_config cfg;
static struct k_spinlock cfg_lock;

static struct sensei_lpwan_config cfg_get(void) {
  k_spinlock_key_t k = k_spin_lock(&cfg_lock);
  struct sensei_lpwan_config c = cfg;

  k_spin_unlock(&cfg_lock, k);
  return c;
}

static void report_error(void) {
  if (hooks.on_error != NULL) {
    hooks.on_error();
  }
}

static void submit_work(struct k_work *w) {
  if (hooks.wq != NULL) {
    k_work_submit_to_queue(hooks.wq, w);
  } else {
    k_work_submit(w);
  }
}

/* ---- hardware ---- */

static const struct gpio_dt_spec pin_event = GPIO_DT_SPEC_GET(LINK_NODE, event_gpios);
static const struct gpio_dt_spec pin_busy = GPIO_DT_SPEC_GET(LINK_NODE, busy_gpios);
static const struct gpio_dt_spec pin_ack = GPIO_DT_SPEC_GET(LINK_NODE, ack_gpios);
static const struct gpio_dt_spec pin_spare = GPIO_DT_SPEC_GET(LINK_NODE, spare_gpios);
static const struct gpio_dt_spec pin_power = GPIO_DT_SPEC_GET(LINK_NODE, power_gpios);
static const struct gpio_dt_spec pin_level = GPIO_DT_SPEC_GET(LINK_NODE, level_shifter_gpios);
static const struct gpio_dt_spec pin_bbmode = GPIO_DT_SPEC_GET(LINK_NODE, bb_mode_gpios);
static const struct gpio_dt_spec pin_gap_sel = GPIO_DT_SPEC_GET(LINK_NODE, spi_gap_sel_gpios);

static const struct spi_dt_spec spi_nrf91 =
    SPI_DT_SPEC_GET(SPI_NODE, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct device *const spi_dev = DEVICE_DT_GET(SPI_BUS);
PINCTRL_DT_DEFINE(SPI_BUS); /* own copy of the pin states: the driver's is static */

/* Longest wait for GAP9 to finish (boot, job, power-off) before an exchange or an LTE
 * transmission gives up. The claims are in sensei_busarb/bus_arb.h. */
#define LPWAN_BUS_WAIT_MS 20000
#define SPI_HOLD_MS 2000

/* ---- shared SPI_A bus ---- */

static struct k_mutex bus_lock;
static atomic_t bus_taken;

int sensei_lpwan_spi_acquire(int32_t timeout_ms) {
  struct spi_config cfg = spi_nrf91.config;
  const struct gpio_dt_spec *cs = &cfg.cs.gpio;
  int64_t end = k_uptime_get() + timeout_ms;
  int err;

  if (k_mutex_lock(&bus_lock, K_MSEC(timeout_ms)) != 0) {
    return -ETIMEDOUT;
  }
  if (!bus_claim_wait(BUS_SPI, SPI_HOLD_MS, (uint32_t)MAX(end - k_uptime_get(), 0))) {
    k_mutex_unlock(&bus_lock);
    return -EBUSY; /* GAP9 owns the bus */
  }
  gpio_pin_set_dt(&pin_gap_sel, 0); /* GAP9 isolated from SPI_A */
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_DEFAULT);
  err = gpio_pin_configure_dt(cs, GPIO_OUTPUT_INACTIVE);
  if (err) {
    bus_release(BUS_SPI);
    k_mutex_unlock(&bus_lock);
    return err;
  }
  atomic_set(&bus_taken, 1);
  return 0;
}

void sensei_lpwan_spi_release(void) {
  const struct gpio_dt_spec *cs = &spi_nrf91.config.cs.gpio;

  if (!atomic_get(&bus_taken)) {
    return;
  }
  (void)gpio_pin_configure_dt(cs, GPIO_DISCONNECTED);
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_SLEEP);
  atomic_set(&bus_taken, 0);
  bus_release(BUS_SPI);
  k_mutex_unlock(&bus_lock);
}

bool sensei_lpwan_spi_in_use(void) { return atomic_get(&bus_taken) != 0; }

/* ---- state ---- */

enum cmd_type { CMD_CONNECT, CMD_DISCONNECT, CMD_POWER, CMD_SEND, CMD_SETTINGS, CMD_PROVISION, CMD_VERSION };

struct cmd {
  enum cmd_type type;
  sensei_Radio radio; /* CONNECT */
  bool on;            /* POWER */
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd), 8, 4);
static K_SEM_DEFINE(event_sem, 0, 1);
static K_SEM_DEFINE(version_sem, 0, 1);
static struct gpio_callback event_cb;

static struct lpwan_master master;
static lpwan_sm_t sm;
static bool shield_on;
static bool link_up;
static bool lte_nb;
static int last_result;
static char fw_str[24], modem_str[24];

static sensei_LoraProvision keys;
static atomic_t keys_pending;

static K_THREAD_STACK_DEFINE(stack, CONFIG_SENSEI_LPWAN_STACK_SIZE);
static struct k_thread thread;

/* Connection in progress (thread owned). */
static bool connecting;

/* Connection attempt over (success, failure, timeout, power-off): the LTE claim ends with it. */
static void connect_end(void) {
  connecting = false;
  bus_release(BUS_LTE_CONN);
}
static int64_t connect_deadline;
static sensei_Radio connect_radio;
/* Send in flight. */
static atomic_t sending;
static void (*send_done)(int err, void *user);
static void *send_user;
static uint32_t send_seq;
static int64_t send_deadline;
static uint8_t send_buf[LTE_MTU];
static size_t send_len;
static sensei_Radio send_radio;
static int64_t idle_deadline; /* 0: no deadline */
static uint32_t uplink_ok, uplink_fail;

static sensei_LpwanVersion version_out;
static atomic_t version_wanted;

/* The app work queue gets completions, downlinks and time. */
struct dl_item {
  struct k_work work;
  sensei_Radio radio;
  uint16_t len;
  uint8_t data[LTE_MTU];
};
K_MEM_SLAB_DEFINE_STATIC(dl_slab, sizeof(struct dl_item), 2, 4);

struct time_item {
  struct k_work work;
  uint32_t unix_s;
  sensei_Radio radio;
};
static struct time_item time_item;

static struct k_work flush_work;
static volatile sensei_Radio up_radio;
static struct k_work send_done_work;
static volatile int send_done_err;

static void dl_work(struct k_work *w) {
  struct dl_item *d = CONTAINER_OF(w, struct dl_item, work);

  if (hooks.on_downlink != NULL) {
    hooks.on_downlink(d->data, d->len, d->radio);
  }
  k_mem_slab_free(&dl_slab, d);
}

static void time_work(struct k_work *w) {
  const struct time_item *t = CONTAINER_OF(w, struct time_item, work);

  if (hooks.on_time != NULL) {
    hooks.on_time(t->unix_s, t->radio);
  }
}

static void flush_fn(struct k_work *w) {
  ARG_UNUSED(w);
  if (hooks.on_up != NULL) {
    hooks.on_up(up_radio);
  }
}

static void send_done_fn(struct k_work *w) {
  void (*cb)(int, void *) = send_done;
  void *u = send_user;
  int err = send_done_err;

  ARG_UNUSED(w);
  send_done = NULL;
  atomic_set(&sending, 0);
  if (cb) {
    cb(err, u);
  }
}

/* Thread: finish the send in flight. */
static void finish_send(int err) {
  if (!atomic_get(&sending)) {
    return;
  }
  bus_release(BUS_LTE_TX);
  if (err) {
    uplink_fail++;
    report_error();
  } else {
    uplink_ok++;
  }
  send_deadline = 0;
  send_done_err = err;
  submit_work(&send_done_work);
}

/* ---- shield power and link ---- */

static int expander_outputs(bool on) {
  int err = 0;

  err |= gpio_pin_configure_dt(
      &pin_bbmode, on ? (IS_ENABLED(CONFIG_SENSEI_LPWAN_BB_MODE_PFM) ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE)
                      : GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_power, on ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_level, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_ack, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_spare, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_busy, GPIO_INPUT);
  return err ? -EIO : 0;
}

/* ---- master ops ---- */

static int op_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
  const struct spi_buf txb = {.buf = (void *)tx, .len = len};
  const struct spi_buf rxb = {.buf = rx, .len = len};
  const struct spi_buf_set txs = {.buffers = &txb, .count = 1};
  const struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};
  int err;

  ARG_UNUSED(c);
  err = sensei_lpwan_spi_acquire(LPWAN_BUS_WAIT_MS);
  if (err) {
    return err;
  }
  err = spi_transceive_dt(&spi_nrf91, &txs, &rxs);
  sensei_lpwan_spi_release();
  return err;
}

static int op_wait_ready(void *c, uint32_t timeout_ms) {
  int64_t end = k_uptime_get() + timeout_ms;

  ARG_UNUSED(c);
  do {
    /* BUSY is on the expander: one I2C read per poll. */
    if (gpio_pin_get_dt(&pin_busy) == 0) {
      return 0;
    }
    k_msleep(1);
  } while (k_uptime_get() < end);
  return -ETIMEDOUT;
}

static bool op_event_pending(void *c) {
  ARG_UNUSED(c);
  return gpio_pin_get_dt(&pin_event) > 0;
}

static uint32_t op_now(void *c) {
  ARG_UNUSED(c);
  return (uint32_t)k_uptime_get();
}

static void op_on_event(void *c, const sensei_LpwanEvent *evt) {
  ARG_UNUSED(c);
  lpwan_sm_event(&sm, evt);
  switch (evt->which_body) {
  case sensei_LpwanEvent_joined_tag:
  case sensei_LpwanEvent_connected_tag:
    LOG_INF("%s", evt->which_body == sensei_LpwanEvent_joined_tag ? "joined" : "connected");
    connect_end();
    up_radio = evt->which_body == sensei_LpwanEvent_joined_tag ? evt->joined.radio : evt->connected.radio;
    submit_work(&flush_work); /* the uplink queue waits for this */
    break;
  case sensei_LpwanEvent_disconnected_tag:
    LOG_INF("disconnected");
    connect_end();
    break;
  case sensei_LpwanEvent_tx_done_tag:
    LOG_INF("tx done, seq %u, %s", (unsigned)evt->tx_done.seq, evt->tx_done.ok ? "ok" : "failed");
    finish_send(evt->tx_done.ok ? 0 : -EIO);
    idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
    break;
  case sensei_LpwanEvent_downlink_rx_tag: {
    struct dl_item *d;
    const sensei_DownlinkRx *r = &evt->downlink_rx;

    if (k_mem_slab_alloc(&dl_slab, (void **)&d, K_NO_WAIT) != 0) {
      LOG_WRN("downlink dropped: no buffer");
      report_error();
      break;
    }
    k_work_init(&d->work, dl_work);
    d->radio = r->radio;
    d->len = r->payload.size;
    memcpy(d->data, r->payload.bytes, d->len);
    submit_work(&d->work);
    idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
    break;
  }
  case sensei_LpwanEvent_network_time_tag:
    time_item.unix_s = evt->network_time.unix_time;
    time_item.radio = evt->network_time.radio;
    submit_work(&time_item.work);
    break;
  case sensei_LpwanEvent_error_tag:
    LOG_WRN("nRF9151 error %d: %s", (int)evt->error.code, evt->error.message);
    report_error();
    if (connecting) {
      connect_end();
    }
    if (evt->error.code == sensei_LpwanResult_LPWAN_NOT_CONFIGURED) {
      last_result = sensei_LpwanResult_LPWAN_NOT_CONFIGURED; /* LoRa keys needed */
    }
    break;
  default:
    break;
  }
}

static const struct lpwan_master_ops mops = {
    .xfer = op_xfer,
    .wait_ready = op_wait_ready,
    .event_pending = op_event_pending,
    .now_ms = op_now,
    .on_event = op_on_event,
};

/* ---- requests ---- */

/* Admit locally (clear errors without SPI traffic), send, and undo the transition on refusal. */
static int request(sensei_LpwanRequest *req, sensei_LpwanResponse *rsp, uint32_t timeout_ms) {
  lpwan_sm_t before = sm;
  sensei_LpwanResult res = lpwan_sm_admit(&sm, req);
  int err;

  if (res != sensei_LpwanResult_LPWAN_OK) {
    last_result = res;
    LOG_WRN("request %u refused locally: %d (%s/%s)", (unsigned)req->which_body, (int)res, lpwan_state_name(sm.state),
            lpwan_tech_name(sm.tech));
    return -EPERM;
  }
  err = lpwan_master_request(&master, req, rsp, timeout_ms);
  if (err) {
    sm = before;
    last_result = err;
    LOG_ERR("request %u failed: %d", (unsigned)req->which_body, err);
    return err;
  }
  last_result = rsp->result;
  if (rsp->result != sensei_LpwanResult_LPWAN_OK) {
    sm = before;
    LOG_WRN("request %u rejected by nRF9151: %d", (unsigned)req->which_body, (int)rsp->result);
    return -EIO;
  }
  return 0;
}

#define REQ_TO CONFIG_SENSEI_LPWAN_REQUEST_TIMEOUT_MS

static int simple_request(pb_size_t tag) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  rq.which_body = tag;
  return request(&rq, &rs, REQ_TO);
}

static int read_version(sensei_LpwanVersion *out) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  int err;

  rq.which_body = sensei_LpwanRequest_get_version_tag;
  err = request(&rq, &rs, REQ_TO);
  if (err == 0 && rs.which_payload == sensei_LpwanResponse_version_tag) {
    *out = rs.version;
    snprintf(fw_str, sizeof(fw_str), "%u.%u.%u", (unsigned)rs.version.firmware.major,
             (unsigned)rs.version.firmware.minor, (unsigned)rs.version.firmware.patch);
    snprintf(modem_str, sizeof(modem_str), "%.23s", rs.version.modem);
  }
  return err;
}

/* ---- power ---- */

static int shield_up(void) {
  int64_t end;

  if (shield_on && link_up) {
    return 0;
  }
  if (!shield_on) {
    int err = expander_outputs(true);

    if (err) {
      LOG_ERR("expander setup failed: %d", err);
      return err;
    }
    /* 3.3 V on, then the level shifter, then SPI. */
    k_msleep(5);
    gpio_pin_set_dt(&pin_level, 1);
    gpio_pin_set_dt(&pin_ack, 1);
    shield_on = true;
    lpwan_sm_init(&sm);
    LOG_INF("shield on");
  }
  end = k_uptime_get() + CONFIG_SENSEI_LPWAN_BOOT_TIMEOUT_MS;
  while (k_uptime_get() < end) {
    sensei_LpwanVersion v;

    if (op_wait_ready(NULL, 100) == 0 && read_version(&v) == 0) {
      link_up = true;
      LOG_INF("nRF9151 firmware %s, modem '%s'", fw_str, modem_str);
      return 0;
    }
    k_msleep(50);
  }
  LOG_ERR("nRF9151 does not answer");
  return -ETIMEDOUT;
}

static void shield_down(void) {
  if (!shield_on) {
    return;
  }
  (void)gpio_pin_configure_dt(&pin_level, GPIO_OUTPUT_INACTIVE);
  (void)gpio_pin_configure_dt(&pin_ack, GPIO_OUTPUT_INACTIVE);
  (void)gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_INACTIVE); /* 3.3 V off */
  shield_on = false;
  link_up = false;
  connect_end();
  idle_deadline = 0;
  lpwan_sm_init(&sm);
  LOG_INF("shield off");
}

static bool gating_wanted(void) {
#ifdef CONFIG_SENSEI_LPWAN_POWER_GATING
  /* LTE keeps the shield on so the modem stays registered (PSM). */
  return sm.tech != LPWAN_TECH_LTE;
#else
  return false;
#endif
}

/* ---- configuration ---- */

static int configure_lte(sensei_Radio radio) {
  const struct sensei_lpwan_config st = cfg_get();
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  sensei_LteConfigure *c = &rq.lte_configure;

  rq.which_body = sensei_LpwanRequest_lte_configure_tag;
  c->mode = radio == sensei_Radio_RADIO_NBIOT ? sensei_LteMode_LTE_NB_IOT : sensei_LteMode_LTE_M;
  c->psm_tau_s = st.lte_psm_tau_s;
  c->psm_active_s = st.lte_psm_active_s;
  if (st.lte_edrx) {
    c->edrx_cycle_ms = CONFIG_SENSEI_LPWAN_EDRX_CYCLE_MS;
    c->edrx_ptw_ms = CONFIG_SENSEI_LPWAN_EDRX_PTW_MS;
  }
  strncpy(c->apn, CONFIG_SENSEI_LPWAN_LTE_APN, sizeof(c->apn) - 1);
  strncpy(c->server_host, st.lte_server, sizeof(c->server_host) - 1);
  c->server_port = st.lte_port;
  c->udp = true;
  lte_nb = c->mode == sensei_LteMode_LTE_NB_IOT;
  return request(&rq, &rs, REQ_TO);
}

static int configure_lora(void) {
  const struct sensei_lpwan_config st = cfg_get();
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  sensei_LoraConfigure *c = &rq.lora_configure;

  rq.which_body = sensei_LpwanRequest_lora_configure_tag;
  c->region = sensei_LoraRegion_REGION_EU868;
  c->lora_class = sensei_LoraClass_CLASS_A;
  c->data_rate = st.lora_dr;
  c->adr = st.lora_adr;
  c->confirmed = st.lora_confirmed;
  c->port = st.lora_port;
  return request(&rq, &rs, REQ_TO);
}

/* Start a connection: bring the shield up, configure, ask the nRF9151 to connect. */
static void do_connect(sensei_Radio radio) {
  lpwan_tech_t want = lpwan_tech_of_radio(radio);
  int err;

  if (want != LPWAN_TECH_LORA && want != LPWAN_TECH_LTE) {
    return;
  }
  if (sm.state == LPWAN_ST_CONNECTED && sm.tech == want &&
      (want != LPWAN_TECH_LTE || lte_nb == (radio == sensei_Radio_RADIO_NBIOT))) {
    return; /* already there */
  }
  if (sm.tech == LPWAN_TECH_BLE) {
    LOG_WRN("BLE holds the radio time");
    return;
  }
  err = shield_up();
  if (err) {
    return;
  }
  if (sm.tech != LPWAN_TECH_NONE && (sm.tech != want || want == LPWAN_TECH_LTE)) {
    /* Another radio, or the other LTE mode, is active: release it first. */
    if (simple_request(sensei_LpwanRequest_lte_disconnect_tag) == 0) {
      (void)lpwan_master_fetch_events(&master, 8);
    }
  }
  if (sm.state != LPWAN_ST_IDLE) {
    return;
  }
  if (want == LPWAN_TECH_LTE) {
    /* No GAP9 job during LTE registration (current peaks). */
    if (!bus_claim_wait(BUS_LTE_CONN, (CONFIG_SENSEI_LPWAN_LTE_CONNECT_TIMEOUT_S + 5) * 1000U, LPWAN_BUS_WAIT_MS)) {
      LOG_WRN("GAP9 busy, LTE connect postponed");
      return;
    }
    if (configure_lte(radio) || simple_request(sensei_LpwanRequest_lte_connect_tag)) {
      bus_release(BUS_LTE_CONN);
      return;
    }
    connect_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_LTE_CONNECT_TIMEOUT_S * 1000LL;
  } else {
    if (configure_lora()) {
      return;
    }
    if (atomic_get(&keys_pending)) {
      sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
      sensei_LpwanResponse rs;

      rq.which_body = sensei_LpwanRequest_lora_provision_tag;
      rq.lora_provision = keys;
      if (request(&rq, &rs, REQ_TO) == 0) {
        atomic_set(&keys_pending, 0);
        memset(&keys, 0, sizeof(keys)); /* the LR1121 holds them now */
      }
    }
    if (simple_request(sensei_LpwanRequest_lora_join_tag)) {
      return;
    }
    connect_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_JOIN_TIMEOUT_S * 1000LL;
  }
  connecting = true;
  connect_radio = radio;
  idle_deadline = 0;
}

static void do_send(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  const struct sensei_lpwan_config st = cfg_get();
  sensei_Send *s = &rq.send;

  if (lpwan_tech_of_radio(send_radio) == LPWAN_TECH_LTE &&
      !bus_claim_wait(BUS_LTE_TX, (CONFIG_SENSEI_LPWAN_TX_TIMEOUT_S + 5) * 1000U, LPWAN_BUS_WAIT_MS)) {
    LOG_WRN("GAP9 busy, LTE send postponed");
    finish_send(-EBUSY);
    return;
  }
  rq.which_body = sensei_LpwanRequest_send_tag;
  s->radio = send_radio;
  s->port = send_radio == sensei_Radio_RADIO_LORA ? st.lora_port : 0;
  s->confirmed = send_radio == sensei_Radio_RADIO_LORA && st.lora_confirmed;
  s->payload.size = (pb_size_t)send_len;
  memcpy(s->payload.bytes, send_buf, send_len);
  idle_deadline = 0;
  if (request(&rq, &rs, REQ_TO) != 0) {
    finish_send(-EIO);
    return;
  }
  send_seq = rq.seq;
  send_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_TX_TIMEOUT_S * 1000LL;
}

static void do_disconnect(void) {
  if (!link_up) {
    return;
  }
  if (simple_request(sensei_LpwanRequest_lte_disconnect_tag) == 0) {
    (void)lpwan_master_fetch_events(&master, 8);
  }
}

static void idle_action(void) {
  idle_deadline = 0;
  if (!shield_on || atomic_get(&sending) || connecting) {
    return;
  }
  if (gating_wanted()) {
    (void)simple_request(sensei_LpwanRequest_sleep_tag);
    shield_down();
  } else if (sm.state == LPWAN_ST_CONNECTED) {
    /* LTE: keep the registration, the modem sleeps in PSM. */
    sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
    sensei_LpwanResponse rs;

    rq.which_body = sensei_LpwanRequest_sleep_tag;
    rq.sleep.deep = 0;
    (void)request(&rq, &rs, REQ_TO);
  }
}

static void handle_cmd(const struct cmd *c) {
  switch (c->type) {
  case CMD_CONNECT:
    do_connect(c->radio);
    break;
  case CMD_DISCONNECT:
    do_disconnect();
    break;
  case CMD_POWER:
    if (c->on) {
      (void)shield_up();
      idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
    } else {
      shield_down();
    }
    break;
  case CMD_SEND:
    do_send();
    break;
  case CMD_PROVISION:
    do_connect(sensei_Radio_RADIO_LORA);
    break;
  case CMD_VERSION:
    if (atomic_get(&version_wanted)) {
      bool was_on = shield_on;

      if (shield_up() == 0 && read_version(&version_out) == 0) {
        atomic_set(&version_wanted, 0);
        k_sem_give(&version_sem);
      }
      if (!was_on && !connecting) {
        idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
      }
    }
    break;
  case CMD_SETTINGS:
    /* The next connect configures from the new settings. A connected LoRa or LTE link is
     * re-configured when it is released. If the radio is no longer an LPWAN one, let go. */
    if (shield_on && gating_wanted() && !atomic_get(&sending) && !connecting) {
      idle_deadline = k_uptime_get() + 1000;
    }
    break;
  }
}

/* ---- thread ---- */

static void check_timers(void) {
  int64_t now = k_uptime_get();

  if (connecting && now > connect_deadline) {
    LOG_ERR("connect timed out");
    connect_end();
    last_result = -ETIMEDOUT;
    report_error();
    /* Let the shield sort itself out: release the technology and power down if allowed. */
    do_disconnect();
    idle_deadline = now + 1000;
  }
  if (atomic_get(&sending) && send_deadline && now > send_deadline) {
    LOG_ERR("no transmit result");
    finish_send(-ETIMEDOUT);
  }
  if (idle_deadline && now > idle_deadline) {
    idle_action();
  }
}

static void thread_fn(void *a, void *b, void *c) {
  struct k_poll_event ev[2];

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    struct cmd cmd;
    int64_t next = INT64_MAX;
    int64_t now = k_uptime_get();

    k_poll_event_init(&ev[0], K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &event_sem);
    k_poll_event_init(&ev[1], K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &cmd_q);
    if (connecting && connect_deadline < next) {
      next = connect_deadline;
    }
    if (send_deadline && atomic_get(&sending) && send_deadline < next) {
      next = send_deadline;
    }
    if (idle_deadline && idle_deadline < next) {
      next = idle_deadline;
    }
    k_timeout_t to = next == INT64_MAX ? K_FOREVER : K_MSEC(MAX(next - now, 1));

    (void)k_poll(ev, 2, to);

    if (shield_on && link_up && (k_sem_take(&event_sem, K_NO_WAIT) == 0)) {
      (void)lpwan_master_fetch_events(&master, 16);
    } else {
      (void)k_sem_take(&event_sem, K_NO_WAIT);
    }
    while (k_msgq_get(&cmd_q, &cmd, K_NO_WAIT) == 0) {
      handle_cmd(&cmd);
    }
    /* An event that arrived during a request is picked up here too. */
    if (shield_on && link_up && op_event_pending(NULL)) {
      (void)lpwan_master_fetch_events(&master, 16);
    }
    check_timers();
  }
}

static void event_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
  ARG_UNUSED(port);
  ARG_UNUSED(cb);
  ARG_UNUSED(pins);
  k_sem_give(&event_sem);
}

static int post(enum cmd_type type, sensei_Radio radio, bool on) {
  struct cmd c = {.type = type, .radio = radio, .on = on};

  return k_msgq_put(&cmd_q, &c, K_NO_WAIT) == 0 ? 0 : -EBUSY;
}

/* ---- radios ---- */

bool sensei_lpwan_radio_ready(sensei_Radio r) {
  lpwan_tech_t want = lpwan_tech_of_radio(r);
  bool lte_ok = want != LPWAN_TECH_LTE || lte_nb == (r == sensei_Radio_RADIO_NBIOT);
  bool ok = link_up && sm.state == LPWAN_ST_CONNECTED && sm.tech == want && lte_ok && !atomic_get(&sending);

  if (!ok && !atomic_get(&sending) && !connecting) {
    (void)post(CMD_CONNECT, r, false); /* wake it; on_up runs when it is up */
  }
  if (ok) {
    idle_deadline = 0; /* keep it up while the queue is being sent */
  }
  return ok;
}

int sensei_lpwan_radio_send(sensei_Radio r, const uint8_t *chunk, size_t len, void (*done)(int, void *), void *user) {
  if (len == 0 || len > sizeof(send_buf)) {
    return -EINVAL;
  }
  if (!atomic_cas(&sending, 0, 1)) {
    return -EBUSY;
  }
  memcpy(send_buf, chunk, len);
  send_len = len;
  send_radio = r;
  send_done = done;
  send_user = user;
  if (post(CMD_SEND, r, false) != 0) {
    atomic_set(&sending, 0);
    return -EBUSY;
  }
  return 0;
}

/* ---- API ---- */

void sensei_lpwan_set_config(const struct sensei_lpwan_config *c) {
  k_spinlock_key_t k = k_spin_lock(&cfg_lock);

  cfg = *c;
  k_spin_unlock(&cfg_lock, k);
  /* The next connect configures from the new values. */
  (void)post(CMD_SETTINGS, sensei_Radio_RADIO_BLE, false);
}

int sensei_lpwan_init(const struct sensei_lpwan_hooks *h, const struct sensei_lpwan_config *c) {
  int err = 0;

  hooks = *h;
  cfg = *c;

  k_mutex_init(&bus_lock);
  k_work_init(&flush_work, flush_fn);
  k_work_init(&send_done_work, send_done_fn);
  k_work_init(&time_item.work, time_work);
  lpwan_sm_init(&sm);
  lpwan_master_init(&master, &mops, NULL);
  master.ready_timeout_ms = 1000;

  if (!device_is_ready(spi_dev) || !gpio_is_ready_dt(&pin_event) || !gpio_is_ready_dt(&pin_busy) ||
      !gpio_is_ready_dt(&pin_power) || !gpio_is_ready_dt(&pin_gap_sel)) {
    LOG_ERR("LPWAN shield devices not ready (is LPWAN_Shield_v1 in the build?)");
    return -ENODEV;
  }
  /* Shield off, SPI pins parked, GAP9 free to use the bus. */
  err |= gpio_pin_configure_dt(&pin_gap_sel, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_event, GPIO_INPUT | GPIO_PULL_DOWN);
  err |= expander_outputs(false);
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_SLEEP);
  (void)gpio_pin_configure_dt(&spi_nrf91.config.cs.gpio, GPIO_DISCONNECTED);
  if (err) {
    LOG_ERR("shield pins failed (expander at 0x20 not answering?)");
    return -EIO;
  }
  gpio_init_callback(&event_cb, event_isr, BIT(pin_event.pin));
  err = gpio_add_callback(pin_event.port, &event_cb);
  err |= gpio_pin_interrupt_configure_dt(&pin_event, GPIO_INT_EDGE_RISING);
  if (err) {
    return -EIO;
  }

  k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), thread_fn, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_SENSEI_LPWAN_THREAD_PRIORITY), 0, K_NO_WAIT);
  k_thread_name_set(&thread, "lpwan");
  LOG_INF("LPWAN client ready, shield off");
  return 0;
}

int sensei_lpwan_lora_provision(const sensei_LoraProvision *k) {
  if (k == NULL) {
    return -EINVAL;
  }
  keys = *k;
  atomic_set(&keys_pending, 1);
  return post(CMD_PROVISION, sensei_Radio_RADIO_LORA, false);
}

int sensei_lpwan_cmd_connect(sensei_Radio radio) { return post(CMD_CONNECT, radio, false); }

int sensei_lpwan_cmd_disconnect(void) { return post(CMD_DISCONNECT, sensei_Radio_RADIO_BLE, false); }

int sensei_lpwan_cmd_power(bool on) { return post(CMD_POWER, sensei_Radio_RADIO_BLE, on); }

int sensei_lpwan_query_version(sensei_LpwanVersion *out, uint32_t timeout_ms) {
  k_sem_reset(&version_sem);
  atomic_set(&version_wanted, 1);
  if (post(CMD_VERSION, sensei_Radio_RADIO_BLE, false) != 0) {
    return -EBUSY;
  }
  if (k_sem_take(&version_sem, K_MSEC(timeout_ms)) != 0) {
    atomic_set(&version_wanted, 0);
    return -ETIMEDOUT;
  }
  *out = version_out;
  return 0;
}

void sensei_lpwan_get_info(struct lpwan_info *i) {
  memset(i, 0, sizeof(*i));
  i->state = sm.state;
  i->tech = sm.tech;
  i->shield_on = shield_on;
  i->link_up = link_up;
  i->lte_mode_nb = lte_nb;
  i->transfers = master.n_transfers;
  i->bad_frames = master.n_bad_frames;
  i->retries = master.n_retries;
  i->events = master.n_events;
  i->uplink_ok = uplink_ok;
  i->uplink_fail = uplink_fail;
  i->last_result = last_result;
  i->provisioned_pending = atomic_get(&keys_pending) != 0;
  strncpy(i->fw, fw_str, sizeof(i->fw) - 1);
  strncpy(i->modem, modem_str, sizeof(i->modem) - 1);
}

const char *sensei_lpwan_state_str(void) {
  if (!shield_on) {
    return "shield off";
  }
  return lpwan_state_name(sm.state);
}
