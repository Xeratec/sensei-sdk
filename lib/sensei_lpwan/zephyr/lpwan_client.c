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
#ifndef CONFIG_SENSEI_LPWAN_SIM
#include <zephyr/drivers/pinctrl.h>
#endif
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>

#include "lpwan_priv.h"
#include "sensei_busarb/bus_arb.h"
#include "sensei_link/frag.h"
#include "sensei_lpwan/lpwan.h"
#include "sensei_lpwan/lpwan_dfu.h"
#include "sensei_lpwan/lpwan_lora.h"
#include "sensei_lpwan/lpwan_master.h"
#include "sensei_lpwan/lpwan_upd.h"
#ifdef CONFIG_SENSEI_LPWAN_SIM
#include "lpwan_sim_master.h"
#endif

LOG_MODULE_REGISTER(sensei_lpwan, CONFIG_SENSEI_LPWAN_LOG_LEVEL);

#ifndef CONFIG_SENSEI_LPWAN_SIM
#define LINK_NODE DT_ALIAS(lpwan_link)
#define SPI_NODE DT_ALIAS(lpwan_spi)
#define SPI_BUS DT_BUS(SPI_NODE)
#endif

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

static void report_power(bool on) {
  if (hooks.on_power != NULL) {
    hooks.on_power(on);
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

/* The simulated node (native_sim, CONFIG_SENSEI_LPWAN_SIM) has no SPI, expander or pins: the link to the shield is a
 * socket (lpwan_sim_master.c). The places that touch the hardware have a second branch below. */
#ifndef CONFIG_SENSEI_LPWAN_SIM
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
#endif                      /* !CONFIG_SENSEI_LPWAN_SIM */

/* Longest wait for GAP9 to finish (boot, job, power-off) before an exchange or an LTE
 * transmission gives up. The claims are in sensei_busarb/bus_arb.h. */
#define LPWAN_BUS_WAIT_MS 20000
#define SPI_HOLD_MS 2000

/* ---- shared SPI_A bus ---- */

/*
 * The pins of the nRF9151 link (SPIM4) are the same as those of GAP9 (SPIS3), switched by SPI_GAP_SEL
 * (P0.20). One side owns the bus at a time. The client takes it for each exchange with the shield and
 * gives it back at once. bus_take() waits up to timeout_ms for the bus (GAP9 off), isolates GAP9
 * (SPI_GAP_SEL low) and connects the SPIM4 pins; bus_give() parks the pins and the chip select (high
 * impedance), so GAP9 can take the bus. Arbitration is in lib/sensei_busarb. Private: the mutex belongs
 * to the thread that took it.
 *
 * While GAP9 holds the bus the shield's level shifter is switched off (level_update()), so that the
 * GAP9 chip select and clock do not reach the nRF9151 SPIS and its MISO does not drive against SPIS3.
 */

static K_SEM_DEFINE(event_sem, 0, 1);
static struct k_mutex bus_lock;
static atomic_t bus_taken;
static bool level_on;             /* level shifter enabled (thread owned) */
static atomic_t gap9_bus_changed; /* BUS_GAP9 was claimed or released: the thread updates the shifter */

static bool shield_on; /* 3.3 V of the shield on (thread owned) */

static void level_update(void);

static int bus_take(int32_t timeout_ms) {
#ifndef CONFIG_SENSEI_LPWAN_SIM
  struct spi_config cfg = spi_nrf91.config;
  const struct gpio_dt_spec *cs = &cfg.cs.gpio;
#endif
  int64_t end = k_uptime_get() + timeout_ms;
  int err = 0;

  if (k_mutex_lock(&bus_lock, K_MSEC(timeout_ms)) != 0) {
    return -ETIMEDOUT;
  }
  if (!bus_claim_wait(BUS_SPI, SPI_HOLD_MS, (uint32_t)MAX(end - k_uptime_get(), 0))) {
    k_mutex_unlock(&bus_lock);
    return -EBUSY; /* GAP9 owns the bus */
  }
  level_update(); /* GAP9 does not hold the bus: the nRF9151 may listen */
#ifndef CONFIG_SENSEI_LPWAN_SIM
  gpio_pin_set_dt(&pin_gap_sel, 0); /* GAP9 isolated from SPI_A */
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_DEFAULT);
  err = gpio_pin_configure_dt(cs, GPIO_OUTPUT_INACTIVE);
#endif
  if (err) {
    bus_release(BUS_SPI);
    k_mutex_unlock(&bus_lock);
    return err;
  }
  atomic_set(&bus_taken, 1);
  return 0;
}

static void bus_give(void) {
  if (!atomic_get(&bus_taken)) {
    return;
  }
#ifndef CONFIG_SENSEI_LPWAN_SIM
  (void)gpio_pin_configure_dt(&spi_nrf91.config.cs.gpio, GPIO_DISCONNECTED);
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_SLEEP);
#endif
  atomic_set(&bus_taken, 0);
  bus_release(BUS_SPI);
  k_mutex_unlock(&bus_lock);
}

bool sensei_lpwan_spi_in_use(void) { return atomic_get(&bus_taken) != 0; }

/* The level shifter (expander P7) is on while the shield is on and GAP9 does not hold the bus. */
static void level_update(void) {
  bool want = shield_on && !bus_is_held(BUS_GAP9);

  if (want != level_on) {
#ifndef CONFIG_SENSEI_LPWAN_SIM
    (void)gpio_pin_configure_dt(&pin_level, want ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
#endif
    level_on = want;
    LOG_DBG("level shifter %s", want ? "on" : "off (GAP9 owns the bus)");
  }
}

/* Called by the bus arbiter, on the thread of the claimer: only wake the client thread. */
static void gap9_listener(enum bus_user u, bool held, void *ctx) {
  ARG_UNUSED(held);
  ARG_UNUSED(ctx);
  if (u == BUS_GAP9) {
    atomic_set(&gap9_bus_changed, 1);
    k_sem_give(&event_sem);
  }
}

/* ---- state ---- */

enum cmd_type {
  CMD_CONNECT,
  CMD_DISCONNECT,
  CMD_POWER,
  CMD_SEND,
  CMD_SETTINGS,
  CMD_PROVISION,
  CMD_VERSION,
  CMD_DFU,
  CMD_EVAL,
  CMD_GNSS,
  CMD_GNSS_CANCEL
};

struct cmd {
  enum cmd_type type;
  sensei_Radio radio; /* CONNECT */
  bool on;            /* POWER */
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd), 8, 4);
static K_SEM_DEFINE(version_sem, 0, 1);
#ifndef CONFIG_SENSEI_LPWAN_SIM
static struct gpio_callback event_cb;
#endif

static struct lpwan_master master;
static lpwan_sm_t sm;
static bool link_up;
static bool proto_bad;          /* the shield seems to speak another protocol version */
static int64_t proto_retry_at;  /* uptime from which the version is checked again */
static int64_t shield_off_at;   /* uptime of the last power-off */
static int64_t shield_retry_at; /* no bring-up before this uptime after a failed one */
static uint32_t recoveries;     /* shield power cycles caused by a lost link */
static bool booting;            /* shield_up() waits for the nRF9151: failures do not count */
static unsigned tx_timeouts;    /* sends in a row that got no tx_done */
static bool lte_nb;
static int last_result;
static char fw_str[24], modem_str[24];

/* LoRaWAN keys: in RAM from the provision call until the shield has them (or KEYS_TRIES failed sends). */
#define KEYS_TRIES 3
static sensei_LoraProvision keys;
static atomic_t keys_pending;
static int keys_tries;

static void wipe_keys(void) {
  memset(&keys, 0, sizeof(keys));
  atomic_set(&keys_pending, 0);
  keys_tries = 0;
}

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
static int64_t send_deadline;
static int64_t send_start;             /* uptime the send was taken by the shield */
static uint32_t send_wait_seq;         /* seq whose tx_done ends the send (the request's, or the frame it adopted) */
static bool deadline_extended;         /* the deadline passed once and the shield held the frame: waited once more */
static bool resend_after_done;         /* the shield held another frame: send this chunk when that one is out */
static unsigned resends;               /* resends of this chunk after a held frame */
static struct lpwan_held held;         /* LoRa frame the shield took and has no result for */
static struct lpwan_held held_prev;    /* `held` before the request in progress */
static volatile int64_t lora_retry_at; /* the shield refuses LoRa sends until this uptime (duty cycle), 0: no */
static struct k_work_delayable retry_work;
static uint32_t send_attempts;         /* transmissions of the send that just ended (TxDone.attempts), 0: not known */
static volatile uint8_t lora_next_max; /* payload the LR1121 reported for the next LoRa uplink, 0: not known */
static uint8_t send_buf[LTE_MTU];
static size_t send_len;
static sensei_Radio send_radio;
static bool send_last;         /* the chunk is the last of its message (RAI) */
static sensei_LteRai send_rai; /* the flag the request carried */
static uint32_t lte_acked, lte_noack;
static bool
    send_unconfirmed; /* the send that just ended went out without the answer (see sensei_lpwan_send_unconfirmed) */
static int64_t idle_deadline; /* 0: no deadline */
static uint32_t uplink_ok, uplink_fail;

static sensei_LpwanVersion version_out;
static atomic_t version_wanted;
static int version_err;

/* The version the shield reported last (sensei_lpwan_shield_version(), Status of the node). */
static struct k_spinlock ver_lock;
static sensei_LpwanVersion shield_ver;
static bool shield_ver_valid;

/* Diagnostics of the shield from its last status (sensei_lpwan_shield_diag()): asked after each version read, so
 * the node learns about a reset of the shield. The shield reports a fatal error only once: it is kept here. */
static struct k_spinlock diag_lock;
static struct sensei_lpwan_shield_diag shield_diag;
static bool shield_diag_valid;

/* Connection evaluation of the LTE modem (CONEVAL): the decision state and the last result. */
static struct k_spinlock eval_lock;
static struct lpwan_eval_state eval;
static struct sensei_lpwan_lte_info eval_info; /* of the last valid result */
static bool eval_info_valid;
static int64_t eval_info_at;
static sensei_RaiSupport rai_seen;
#define EVAL_TIMEOUT_MS 30000u /* the shield evaluates for seconds at most */

/* GNSS fix of the shield (lpwan_gnss.h). gnss_pending: from the call to the callback, it keeps LTE out of the way (the
 * rule is in lpwan_gnss.h). gnss_open: the thread works on it, from the start of do_gnss() to gnss_finish(). */
static atomic_t gnss_pending;
static atomic_t gnss_cancel_flag; /* cancel asked before the request went out */
static bool gnss_open;            /* thread owned */
static int64_t gnss_deadline;     /* result not later than this (thread owned), 0: the request has not gone out */
static uint32_t gnss_seq;         /* seq of the gnss_fix request, the result event carries it */
static uint32_t gnss_timeout;     /* s, as requested */
static sensei_lpwan_gnss_done_fn gnss_done;
static void *gnss_user;
static sensei_GnssFixResult gnss_res;
static int gnss_err;
static struct k_work gnss_work;

/* Update of the shield: the session holds the link (no data uplinks, no power gating) from dfu_begin until it ends
 * (apply, abort, an idle or too long session, or an error that ends it). One request at a time. The rules and the
 * times (what the client remembers about swaps, the trial, the session) are in sensei_lpwan/lpwan_upd.h. */
#define DFU_READY_MS 8000 /* longest the shield may take for one request (a flash erase of the padding) */
static struct lpwan_upd upd;
static atomic_t dfu_active;
static bool dfu_bus;                /* BUS_DFU claimed (thread owned) */
static int64_t dfu_start;           /* uptime the session began (thread owned) */
static int64_t dfu_last;            /* uptime of the last answer: the idle time runs from it (thread owned) */
static int64_t dfu_asked;           /* uptime of the last request, answered or not: the modem polls follow it */
static bool dfu_modem_busy;         /* the modem erases or updates: the client asks the shield itself */
static sensei_DfuTarget dfu_target; /* target of the last answer */
static int64_t dfu_cooldown_until;  /* no new transfer before this uptime (after a too long session) */
static int64_t trial_poll_at;       /* next look at an image on trial (thread owned) */
static atomic_t dfu_busy;           /* a job is queued or running */
static struct dfu_job {
  sensei_LpwanRequest req;
  struct sensei_lpwan_dfu_result res;
  sensei_lpwan_dfu_done_fn done;
  void *user;
  int err;
  struct k_work work;
} dfu_job;

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
static atomic_t send_finishing; /* finish_send() ran, send_done_fn() has not yet */

static void dl_work(struct k_work *w) {
  struct dl_item *d = CONTAINER_OF(w, struct dl_item, work);

  /* The ack of the collector (3 bytes, count 0) is no fragment of a downlink. The shield drops it; one that is older
   * than that logic passes it on, and the node would count it as a broken downlink. */
  if (sensei_frag_is_ack(d->data, d->len, NULL, NULL)) {
    LOG_DBG("ack dropped");
  } else if (hooks.on_downlink != NULL) {
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

/* The duty-cycle refusal of the shield is over: the app may send again. */
static void retry_fn(struct k_work *w) {
  ARG_UNUSED(w);
  lora_retry_at = 0;
  if (hooks.on_up != NULL) {
    hooks.on_up(sensei_Radio_RADIO_LORA);
  }
}

static void send_done_fn(struct k_work *w) {
  void (*cb)(int, void *) = send_done;
  void *u = send_user;
  int err = send_done_err;

  ARG_UNUSED(w);
  send_done = NULL;
  atomic_set(&send_finishing, 0);
  atomic_set(&sending, 0);
  if (cb) {
    cb(err, u);
  }
}

/* Thread: finish the send in flight. */
static void finish_send(int err) {
  if (!atomic_get(&sending) || !atomic_cas(&send_finishing, 0, 1)) {
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
  deadline_extended = false;
  resend_after_done = false;
  resends = 0;
  /* The shield goes to sleep after the idle time, also when this send failed or timed out. */
  idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
  send_done_err = err;
  submit_work(&send_done_work);
}

/* ---- shield power and link ---- */

static int expander_outputs(bool on) {
#ifdef CONFIG_SENSEI_LPWAN_SIM
  level_on = false;
  return lpwan_sim_master_power(on); /* the supply of the shield is the connection */
#else
  int err = 0;

  err |= gpio_pin_configure_dt(
      &pin_bbmode, on ? (IS_ENABLED(CONFIG_SENSEI_LPWAN_BB_MODE_PFM) ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE)
                      : GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_power, on ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_level, GPIO_OUTPUT_INACTIVE);
  level_on = false;
  err |= gpio_pin_configure_dt(&pin_ack, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_spare, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_busy, GPIO_INPUT);
  return err ? -EIO : 0;
#endif
}

static int post(enum cmd_type type, sensei_Radio radio, bool on);

/* ---- master ops ---- */

static int op_xfer(void *c, const uint8_t *tx, uint8_t *rx, size_t len) {
#ifndef CONFIG_SENSEI_LPWAN_SIM
  const struct spi_buf txb = {.buf = (void *)tx, .len = len};
  const struct spi_buf rxb = {.buf = rx, .len = len};
  const struct spi_buf_set txs = {.buffers = &txb, .count = 1};
  const struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};
#endif
  int err;

  ARG_UNUSED(c);
  err = bus_take(LPWAN_BUS_WAIT_MS);
  if (err) {
    return err;
  }
#ifdef CONFIG_SENSEI_LPWAN_SIM
  err = lpwan_sim_master_xfer(tx, rx, len);
#else
  err = spi_transceive_dt(&spi_nrf91, &txs, &rxs);
#endif
  bus_give();
  return err;
}

static int op_wait_ready(void *c, uint32_t timeout_ms) {
  int64_t start = k_uptime_get();
  int64_t end = start + timeout_ms;

  ARG_UNUSED(c);
  do {
    /* BUSY is on the expander: one I2C read per poll. A slave that does not answer within 50 ms is
     * slow or gone: poll every 10 ms then, not every ms. */
#ifdef CONFIG_SENSEI_LPWAN_SIM
    if (!lpwan_sim_master_busy()) {
#else
    if (gpio_pin_get_dt(&pin_busy) == 0) {
#endif
      return 0;
    }
    k_msleep(k_uptime_get() - start < 50 ? 1 : 10);
  } while (k_uptime_get() < end);
  return -ETIMEDOUT;
}

static bool op_event_pending(void *c) {
  ARG_UNUSED(c);
#ifdef CONFIG_SENSEI_LPWAN_SIM
  return lpwan_sim_master_event();
#else
  return gpio_pin_get_dt(&pin_event) > 0;
#endif
}

static uint32_t op_now(void *c) {
  ARG_UNUSED(c);
  return (uint32_t)k_uptime_get();
}

/* ---- connection evaluation ---- */

static void eval_flush(void) {
  up_radio = lte_nb ? sensei_Radio_RADIO_NBIOT : sensei_Radio_RADIO_LTEM;
  submit_work(&flush_work); /* the uplink queue asks again */
}

static void eval_result(const sensei_ConnEval *c) {
  k_spinlock_key_t k = k_spin_lock(&eval_lock);
  uint32_t now = (uint32_t)k_uptime_get();

  lpwan_eval_result(&eval, now, c->valid, lpwan_energy_is_bad(c->energy));
  if (c->rai_support != sensei_RaiSupport_RAI_UNKNOWN) {
    rai_seen = c->rai_support;
  }
  if (c->valid) {
    eval_info.energy = c->energy;
    eval_info.rsrp_valid = c->rsrp_valid;
    eval_info.rsrp_dbm = c->rsrp_valid ? (int16_t)c->rsrp_dbm : 0;
    eval_info.ce_level = (uint8_t)MIN(c->ce_level, 3u);
    eval_info_at = k_uptime_get();
    eval_info_valid = true;
  }
  k_spin_unlock(&eval_lock, k);
  LOG_INF("LTE evaluation: %s, energy %d, RSRP %s%d dBm, CE level %u, RAI of the cell %d",
          c->valid ? "valid" : "no result", (int)c->energy, c->rsrp_valid ? "" : "invalid ", (int)c->rsrp_dbm,
          (unsigned)c->ce_level, (int)c->rai_support);
  eval_flush();
}

/* The request failed (not connected, no answer): count it as an evaluation without result. It is asked for again
 * after SENSEI_LPWAN_EVAL_RETRY_S. `unsupported`: the shield refused it as unsupported (an older shield, no CONEVAL in
 * its build): not asked again for an hour. */
static void eval_failed(bool unsupported) {
  k_spinlock_key_t k = k_spin_lock(&eval_lock);

  if (unsupported) {
    lpwan_eval_unsupported(&eval, (uint32_t)k_uptime_get());
  } else {
    lpwan_eval_result(&eval, (uint32_t)k_uptime_get(), false, false);
  }
  k_spin_unlock(&eval_lock, k);
  eval_flush();
}

/* ---- GNSS fix ---- */

/* The fix is over (a result, a verdict of the client, a lost link): the callback runs on the hook work queue. */
static void gnss_finish(int err, sensei_GnssFixStatus status, const sensei_GnssFixResult *res) {
  if (!gnss_open) {
    return;
  }
  gnss_open = false;
  gnss_deadline = 0;
  memset(&gnss_res, 0, sizeof(gnss_res));
  if (res != NULL) {
    gnss_res = *res;
  }
  gnss_res.status = status;
  gnss_err = err;
  if (shield_on && !connecting && !atomic_get(&sending) && !atomic_get(&dfu_active)) {
    idle_deadline =
        k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL; /* a LoRa or BLE node powers the shield down */
  }
  submit_work(&gnss_work);
}

static void gnss_done_fn(struct k_work *w) {
  sensei_lpwan_gnss_done_fn cb = gnss_done;
  void *u = gnss_user;
  sensei_GnssFixResult r = gnss_res;
  int err = gnss_err;

  ARG_UNUSED(w);
  gnss_done = NULL;
  atomic_set(&gnss_pending, 0); /* before the callback: it may ask for the next fix */
  if (cb != NULL) {
    cb(err, &r, u);
  }
  if (hooks.on_up != NULL) {
    hooks.on_up(sensei_Radio_RADIO_BLE); /* LTE was kept out of the way: the uplink queue may go on */
  }
}

static void gnss_event(const sensei_GnssFixResult *r) {
  if (!gnss_open || r->seq != gnss_seq) {
    LOG_WRN("GNSS result of seq %u ignored (%s)", (unsigned)r->seq, gnss_open ? "another fix" : "none waits");
    return;
  }
  LOG_INF("GNSS: %s, %u satellites, %u ms", lpwan_gnss_status_name(r->status), (unsigned)r->sats, (unsigned)r->ttff_ms);
  gnss_finish(0, r->status, r);
}

static void op_on_event(void *c, const sensei_LpwanEvent *evt) {
  /* False: the event does not fit (the radio is not the active one, a tx_done of another send). */
  bool applied = lpwan_sm_event(&sm, evt);

  ARG_UNUSED(c);
  switch (evt->which_body) {
  case sensei_LpwanEvent_joined_tag:
  case sensei_LpwanEvent_connected_tag:
    if (!applied) {
      LOG_WRN("%s event ignored (state %s/%s)",
              evt->which_body == sensei_LpwanEvent_joined_tag ? "joined" : "connected", lpwan_state_name(sm.state),
              lpwan_tech_name(sm.tech));
      break;
    }
    LOG_INF("%s", evt->which_body == sensei_LpwanEvent_joined_tag ? "joined" : "connected");
    lora_next_max = 0; /* a new session: the first chunk follows the data rate */
    connect_end();
    up_radio = evt->which_body == sensei_LpwanEvent_joined_tag ? evt->joined.radio : evt->connected.radio;
    submit_work(&flush_work); /* the uplink queue waits for this */
    break;
  case sensei_LpwanEvent_disconnected_tag:
    if (!applied) {
      break;
    }
    LOG_INF("disconnected");
    connect_end();
    break;
  case sensei_LpwanEvent_tx_done_tag:
    lpwan_held_result(&held, evt->tx_done.seq); /* the frame is out (or lost): not held any more */
    if (!applied || !atomic_get(&sending)) {
      /* A late or second result: the send was given up, ended or is another one (the machine knows its seq). */
      LOG_WRN("tx done, seq %u ignored (%s)", (unsigned)evt->tx_done.seq, lpwan_state_name(sm.state));
      break;
    }
    LOG_INF("tx done, seq %u, %s%s", (unsigned)evt->tx_done.seq, evt->tx_done.ok ? "ok" : "failed",
            evt->tx_done.acked ? ", answered" : (evt->tx_done.unconfirmed ? ", sent, unconfirmed" : ""));
    send_unconfirmed = evt->tx_done.ok && evt->tx_done.unconfirmed;
    if (send_radio != sensei_Radio_RADIO_LORA) {
      if (evt->tx_done.acked) {
        lte_acked++;
      } else if (send_unconfirmed) {
        /* The shield sent the datagram, waited for the answer, and none came. Not a failed send (ok is false), and not
         * a shield that did not wait (an older app, RAI off, a chunk without the answer key): it sets neither flag. */
        lte_noack++;
      }
    }
    if (send_radio == sensei_Radio_RADIO_LORA) {
      lora_next_max = (uint8_t)MIN(evt->tx_done.next_max_payload, UINT8_MAX);
    }
    tx_timeouts = 0; /* the shield answers */
    if (resend_after_done) {
      /* The frame of an earlier send was held: it is out now, send this chunk. */
      resend_after_done = false;
      deadline_extended = false;
      send_deadline = 0;
      if (post(CMD_SEND, send_radio, false) == 0) {
        break;
      }
      finish_send(-EBUSY);
      break;
    }
    send_attempts = sm.send_attempts;
    finish_send(evt->tx_done.ok ? 0 : -EIO);
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
  case sensei_LpwanEvent_conn_eval_tag:
    eval_result(&evt->conn_eval);
    break;
  case sensei_LpwanEvent_gnss_result_tag:
    gnss_event(&evt->gnss_result);
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

static void link_recover(const char *why);
static void read_diag(void);

/*
 * Admit locally (clear errors without SPI traffic), send, and undo the admission on failure.
 * A shield that says "not connected" or "not joined" to a request our state machine allowed has
 * restarted or lost the connection: start again from idle, so the next use connects again.
 * Several failed requests in a row power-cycle the shield (link_recover).
 */
static int request(sensei_LpwanRequest *req, sensei_LpwanResponse *rsp, uint32_t timeout_ms) {
  lpwan_sm_t before = sm;
  sensei_LpwanResult res;
  lpwan_sm_t admitted;
  int err;

  if (!shield_on) {
    last_result = -ENOTCONN;
    return -ENOTCONN; /* power-cycled or never started: shield_up() first */
  }
  if (req->which_body == sensei_LpwanRequest_send_tag) {
    req->seq = lpwan_master_next_seq(&master); /* the machine must know it before a tx_done can arrive */
    send_wait_seq = req->seq;
  }
  res = lpwan_sm_admit(&sm, req);
  if (res != sensei_LpwanResult_LPWAN_OK) {
    last_result = res;
    LOG_WRN("request %u refused locally: %d (%s/%s)", (unsigned)req->which_body, (int)res, lpwan_state_name(sm.state),
            lpwan_tech_name(sm.tech));
    return -EPERM;
  }
  admitted = sm;
  if (req->which_body == sensei_LpwanRequest_send_tag && req->send.radio == sensei_Radio_RADIO_LORA) {
    /* From now on the shield may hold the frame (also if the answer is lost): a tx_done for it can come at once. */
    held_prev = held;
    lpwan_held_arm(&held, req->seq, req->send.payload.bytes, req->send.payload.size);
  }
  err = lpwan_master_request(&master, req, rsp, timeout_ms);
  if (err) {
    lpwan_sm_revert(&sm, &before, &admitted);
    last_result = err;
    if (err == -EBUSY) {
      master.failures = 0; /* GAP9 owns the bus: not the shield's fault */
    }
    if (booting) {
      LOG_DBG("request %u failed: %d (%u in a row)", (unsigned)req->which_body, err, (unsigned)master.failures);
    } else {
      LOG_ERR("request %u failed: %d (%u in a row)", (unsigned)req->which_body, err, (unsigned)master.failures);
    }
    if (!booting && master.failures >= CONFIG_SENSEI_LPWAN_LINK_FAILURES) {
      link_recover("no answer");
    }
    return err;
  }
  last_result = rsp->result;
  if (rsp->result != sensei_LpwanResult_LPWAN_OK && req->which_body == sensei_LpwanRequest_send_tag &&
      req->send.radio == sensei_Radio_RADIO_LORA) {
    held = held_prev; /* refused: this chunk was not taken; an earlier frame may still be held */
  }
  if (rsp->result != sensei_LpwanResult_LPWAN_OK) {
    if (rsp->result == sensei_LpwanResult_LPWAN_NOT_CONNECTED || rsp->result == sensei_LpwanResult_LPWAN_NOT_JOINED) {
      LOG_WRN("nRF9151 is not connected, but we thought so: start again");
      lpwan_sm_init(&sm);
      lpwan_held_clear(&held); /* it has restarted: nothing is held */
      connect_end();
      {
        int answer = last_result;

        read_diag(); /* why it restarted: its reset cause and fault record (a restart reads no version) */
        last_result = answer;
      }
    } else {
      lpwan_sm_revert(&sm, &before, &admitted);
    }
    LOG_WRN("request %u rejected by nRF9151: %d", (unsigned)req->which_body, (int)rsp->result);
    return -EIO;
  }
  return 0;
}

#define REQ_TO CONFIG_SENSEI_LPWAN_REQUEST_TIMEOUT_MS
/* The client gives a send up this long after the time the nRF9151 expects (it ends the send first, after 5 s). */
#define SEND_CLIENT_EXTRA_MS 20000U

static int simple_request(pb_size_t tag) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  rq.which_body = tag;
  return request(&rq, &rs, REQ_TO);
}

/* Refuse the shield for a while. Not for good: two damaged version bytes look like another version, and the
 * shield may have been updated. The check is made again after PROTO_RETRY_MS. */
#define PROTO_RETRY_MS (3600 * 1000LL)

static void mark_proto_bad(void) {
  proto_bad = true;
  proto_retry_at = k_uptime_get() + PROTO_RETRY_MS;
}

/* Reset cause, boot count and fatal error of the shield (LpwanStatus). A shield without the fields answers zeros.
 * Read after every version read, after a restart that the shield reported (NOT_CONNECTED) and before the client cuts
 * the power of a shield that does not answer (link_recover). The shield keeps its fault record until the diag_ack
 * that follows, so a lost answer does not lose it. */
static void read_diag(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  static bool busy; /* the requests below may report a restart again */

  if (busy) {
    return;
  }
  busy = true;
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  if (request(&rq, &rs, REQ_TO) != 0 || rs.which_payload != sensei_LpwanResponse_status_tag) {
    busy = false;
    return;
  }
  k_spinlock_key_t k = k_spin_lock(&diag_lock);

  shield_diag.reset_cause = rs.status.reset_cause;
  shield_diag.boot_count = rs.status.boot_count;
  shield_diag.counters_valid = rs.status.has_rrc_connected_ms && rs.status.has_attaches && rs.status.has_psm_entries;
  shield_diag.rrc_connected_ms = rs.status.rrc_connected_ms;
  shield_diag.attaches = rs.status.attaches;
  shield_diag.psm_entries = rs.status.psm_entries;
  shield_diag.gnss_valid = rs.status.has_gnss_attempts && rs.status.has_gnss_fixes && rs.status.has_gnss_on_ms;
  shield_diag.gnss_attempts = rs.status.gnss_attempts;
  shield_diag.gnss_fixes = rs.status.gnss_fixes;
  shield_diag.gnss_on_ms = rs.status.gnss_on_ms;
  if (rs.status.has_fault) {
    shield_diag.has_fault = true;
    shield_diag.fault = rs.status.fault;
  }
  shield_diag_valid = true;
  k_spin_unlock(&diag_lock, k);
  if (rs.status.has_fault) {
    LOG_WRN("shield: last run ended with a fatal error, reason %u, pc 0x%08x, thread %s",
            (unsigned)rs.status.fault.reason, (unsigned)rs.status.fault.pc, rs.status.fault.thread);
    memset(&rq, 0, sizeof(rq));
    rq.which_body = sensei_LpwanRequest_diag_ack_tag; /* stored: the shield may forget it (an older shield refuses) */
    (void)request(&rq, &rs, REQ_TO);
  }
  busy = false;
}

static int read_version(sensei_LpwanVersion *out) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  int err;

  rq.which_body = sensei_LpwanRequest_get_version_tag;
  err = request(&rq, &rs, REQ_TO);
  if (err == -EPROTONOSUPPORT) {
    mark_proto_bad(); /* another SPI frame version (or two damaged version bytes) */
  }
  if (err == 0 && rs.which_payload == sensei_LpwanResponse_version_tag) {
    if (rs.version.protocol != LPWAN_PROTOCOL_VERSION) {
      LOG_ERR("nRF9151 speaks LPWAN protocol %u, this client %u: update the shield firmware",
              (unsigned)rs.version.protocol, (unsigned)LPWAN_PROTOCOL_VERSION);
      mark_proto_bad();
      return -EPROTONOSUPPORT;
    }
    *out = rs.version;
    lpwan_upd_version(&upd, &rs.version, k_uptime_get());
    k_spinlock_key_t k = k_spin_lock(&ver_lock);

    shield_ver = rs.version;
    shield_ver_valid = true;
    k_spin_unlock(&ver_lock, k);
    snprintf(fw_str, sizeof(fw_str), "%u.%u.%u", (unsigned)rs.version.firmware.major,
             (unsigned)rs.version.firmware.minor, (unsigned)rs.version.firmware.patch);
    snprintf(modem_str, sizeof(modem_str), "%.23s", rs.version.modem);
    read_diag();
  }
  return err;
}

/* ---- power ---- */

static void shield_down(void);

/* A shield image on trial (after an update) confirms itself only when we tell it that the handshake worked: the
 * protocol version matched, it can be updated again and answers dfu_status. We own the trial: dfu_confirm (with the
 * image id) goes out at every look, every LPWAN_UPD_POLL_MS, and the shield stays on until `trial` is false. Without
 * a confirm MCUboot goes back to the old image at the next reset or power cycle. */
static void trial_step(const sensei_LpwanVersion *v) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  bool status_ok;

  if (!lpwan_upd_confirm_due(&upd)) {
    return;
  }
  trial_poll_at = k_uptime_get() + LPWAN_UPD_POLL_MS;
  rq.which_body = sensei_LpwanRequest_dfu_status_tag;
  status_ok = request(&rq, &rs, REQ_TO + 1500) == 0 && rs.which_payload == sensei_LpwanResponse_dfu_tag;
  if (!lpwan_upd_may_confirm(&upd, v, LPWAN_PROTOCOL_VERSION, status_ok)) {
    LOG_WRN("shield image %u.%u.%u is on trial and is not accepted (protocol %u, targets 0x%x, status %s)",
            (unsigned)v->firmware.major, (unsigned)v->firmware.minor, (unsigned)v->firmware.patch,
            (unsigned)v->protocol, (unsigned)v->dfu_targets, status_ok ? "ok" : "failed");
    return;
  }
  LOG_INF("shield image %u.%u.%u is on trial: confirming", (unsigned)v->firmware.major, (unsigned)v->firmware.minor,
          (unsigned)v->firmware.patch);
  memset(&rq, 0, sizeof(rq));
  rq.which_body = sensei_LpwanRequest_dfu_confirm_tag;
  rq.dfu_confirm.image_id = v->image_id;
  (void)request(&rq, &rs, REQ_TO);
}

/* Look at the shield: version, and the trial of a new image (timer). */
static void trial_poll(int64_t now) {
  sensei_LpwanVersion v;

  trial_poll_at = now + LPWAN_UPD_POLL_MS;
  if (read_version(&v) != 0) {
    return;
  }
  if (!lpwan_upd_keep_power(&upd, now)) {
    idle_deadline = now + 1000; /* the image did not confirm in time: it reverts, let the power go */
  } else {
    trial_step(&v);
  }
}

/* After a failed bring-up the next one waits, so a dead shield does not keep the thread and the
 * rail busy. Power-off time: the nRF9151 must lose its supply to reset. */
#define SHIELD_OFF_MS 300
#define SHIELD_RETRY_MS 15000

static int shield_up_for(uint32_t boot_timeout_ms);

static int shield_up(void) { return shield_up_for(CONFIG_SENSEI_LPWAN_BOOT_TIMEOUT_MS); }

static int shield_up_for(uint32_t boot_timeout_ms) {
  uint32_t wait = lpwan_upd_boot_timeout(&upd, boot_timeout_ms); /* a swap may be running: wait for it, no power cut */
  bool waited_long = wait >= LPWAN_UPD_SWAP_TIMEOUT_MS;
  int64_t end, hard_end, renew = 0;

  if (proto_bad) {
    if (k_uptime_get() < proto_retry_at) {
      return -EPROTONOSUPPORT;
    }
    proto_bad = false; /* ask again, from a fresh start of the shield */
    shield_down();
  }
  if (shield_on && link_up) {
    return 0;
  }
  if (!shield_on) {
    int err;

    if (k_uptime_get() < shield_retry_at) {
      return -EAGAIN;
    }
    while (k_uptime_get() - shield_off_at < SHIELD_OFF_MS) {
      k_msleep(10);
    }
    err = expander_outputs(true);
    if (err) {
      LOG_ERR("expander setup failed: %d", err);
      return err;
    }
    /* 3.3 V on, then the level shifter (unless GAP9 has the bus), then SPI. */
    k_msleep(5);
    shield_on = true;
    report_power(true);
    level_update();
#ifndef CONFIG_SENSEI_LPWAN_SIM
    gpio_pin_set_dt(&pin_ack, 1);
#endif
    LOG_INF("shield on");
  }
  /* A new session: the slave has no memory of the old one. */
  lpwan_sm_init(&sm);
  lpwan_master_reset(&master);
  master.failures = 0;
  booting = true;
  end = k_uptime_get() + wait;
  hard_end = k_uptime_get() + 2 * (int64_t)wait;
  while (k_uptime_get() < end) {
    sensei_LpwanVersion v;

    if (dfu_bus && k_uptime_get() >= renew) { /* the lease of BUS_DFU must outlast a swap */
      (void)bus_claim(BUS_DFU, LPWAN_UPD_BUS_HOLD_MS);
      renew = k_uptime_get() + 5000;
    }
    if (op_wait_ready(NULL, 100) == 0) {
      int err = read_version(&v);

      if (err == 0) {
        booting = false;
        link_up = true;
        LOG_INF("nRF9151 firmware %s, modem '%s'", fw_str, modem_str);
        lpwan_upd_bringup_end(&upd, waited_long, true);
        trial_step(&v);
        return 0;
      }
      if (err == -EPROTONOSUPPORT) {
        break; /* no use to wait */
      }
      if (err == -EBUSY) {
        end = MIN(k_uptime_get() + (int64_t)wait, hard_end); /* GAP9 had the bus: not the shield's fault */
      }
    }
    k_msleep(50);
  }
  booting = false;
  lpwan_upd_bringup_end(&upd, waited_long || proto_bad, false); /* a shield that answers is not swapping */
  if (proto_bad) {
    LOG_ERR("nRF9151 protocol mismatch");
  } else {
    LOG_ERR("nRF9151 does not answer: power-cycle");
  }
  report_error();
  shield_down(); /* power off now; the next bring-up starts from a reset shield */
  shield_retry_at = k_uptime_get() + SHIELD_RETRY_MS;
  return proto_bad ? -EPROTONOSUPPORT : -ETIMEDOUT;
}

static void shield_down(void) {
  if (!shield_on) {
    return;
  }
  finish_send(-ENOTCONN);                                                /* a send in flight is gone with the power */
  gnss_finish(-ENOTCONN, sensei_GnssFixStatus_GNSS_FIX_CANCELLED, NULL); /* so is a fix */
#ifdef CONFIG_SENSEI_LPWAN_SIM
  (void)lpwan_sim_master_power(false); /* 3.3 V off */
#else
  (void)gpio_pin_configure_dt(&pin_level, GPIO_OUTPUT_INACTIVE);
  (void)gpio_pin_configure_dt(&pin_ack, GPIO_OUTPUT_INACTIVE);
  (void)gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_INACTIVE); /* 3.3 V off */
#endif
  level_on = false;
  lpwan_upd_power_cut(&upd); /* an image on trial goes back: a swap at power-up */
  shield_on = false;
  report_power(false);
  shield_off_at = k_uptime_get();
  link_up = false;
  lora_next_max = 0;
  lpwan_held_clear(&held); /* a frame in the LR1121 is gone with the power */
  connect_end();
  idle_deadline = 0;
  lpwan_sm_init(&sm);
  LOG_INF("shield off");
}

/* The link is lost (the nRF9151 hangs or resets, requests fail in a row, no tx_done): give up what is in
 * flight and cut the shield power. The next use starts the shield again. LoRaWAN keeps its session
 * in the LR1121, LTE registers again. */
static void link_recover(const char *why) {
  LOG_ERR("link lost (%s): power-cycling the shield", why);
  recoveries++;
  finish_send(-EIO);
  report_error();
  /* A shield that faulted resets itself and keeps its fault record in RAM, which the power cut would erase. Give it
   * the time to boot and ask once for its status. `booting` keeps a failure here from starting another recovery. */
  if (shield_on && !booting) {
    booting = true;
    if (op_wait_ready(NULL, CONFIG_SENSEI_LPWAN_BOOT_TIMEOUT_MS) == 0) {
      read_diag();
    }
    booting = false;
  }
  shield_down();
  tx_timeouts = 0;
  shield_retry_at = k_uptime_get() + 1000; /* give the cycle a moment, the uplink retries */
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
  if (atomic_get(&dfu_active)) {
    return; /* the update holds the link; on_up comes when it ends */
  }
  if (want == LPWAN_TECH_LTE && atomic_get(&gnss_pending)) {
    return; /* a fix and an LTE connect exclude each other; on_up comes when the fix is over */
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
        wipe_keys(); /* the LR1121 holds them now */
      } else if (++keys_tries >= KEYS_TRIES) {
        LOG_ERR("LoRaWAN keys could not be sent %d times: dropped, provision again", KEYS_TRIES);
        wipe_keys();
        report_error();
      }
      memset(&rq, 0, sizeof(rq)); /* the copy on the stack and the frame in the master buffer */
      lpwan_master_wipe(&master);
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

/* The deadline of the send in flight: the time the nRF9151 expects plus a margin. Until twice that time the LR1121 may
 * still hold a LoRa frame (the nRF9151 resets it then): the shield stays on. */
static void arm_deadline(uint32_t expected_ms) {
  uint32_t period = lpwan_send_limit_ms(expected_ms, CONFIG_SENSEI_LPWAN_TX_TIMEOUT_S * 1000U, SEND_CLIENT_EXTRA_MS);
  int64_t now = k_uptime_get();

  send_start = now;
  send_deadline = now + period;
  lpwan_held_expect(&held, (uint32_t)now, period);
}

/* The nRF9151 answered a LoRa send with BUSY. If it holds the frame of an earlier send (the answer says which), the
 * LR1121 cannot take it back: wait for its tx_done, and do not send the same chunk twice. If it holds nothing and
 * gives a time, the duty-cycle wait is longer than a send may take (or MAC answers are flushed): refuse until then. */
static void busy_answer(const sensei_LpwanResponse *rs) {
  uint32_t queued_seq = rs->which_payload == sensei_LpwanResponse_send_tag ? rs->send.queued_seq : 0;
  uint32_t after_ms = rs->which_payload == sensei_LpwanResponse_send_tag ? rs->send.expected_ms : 0;
  enum lpwan_busy_action act = lpwan_held_busy(&held, send_buf, send_len, queued_seq);

  if (act == LPWAN_BUSY_RETRY_LATER) {
    if (after_ms != 0) {
      lora_retry_at = k_uptime_get() + after_ms;
      if (hooks.wq != NULL) {
        (void)k_work_reschedule_for_queue(hooks.wq, &retry_work, K_MSEC(after_ms));
      } else {
        (void)k_work_reschedule(&retry_work, K_MSEC(after_ms));
      }
      LOG_WRN("LoRa send refused for %u s (duty cycle)", (unsigned)(after_ms / 1000));
    }
    finish_send(after_ms != 0 ? -EAGAIN : -EIO);
    return;
  }
  if (act == LPWAN_BUSY_WAIT_RESEND && ++resends > 2) {
    finish_send(-EBUSY); /* the shield keeps holding other frames: let the app retry */
    return;
  }
  LOG_INF("the nRF9151 holds frame %u (%s): waiting for its result", (unsigned)queued_seq,
          act == LPWAN_BUSY_ADOPT ? "this chunk" : "another one");
  /* The shield's state machine waits for tx_done of that seq: so does ours. */
  sm.state = LPWAN_ST_SENDING;
  sm.send_seq = queued_seq;
  send_wait_seq = queued_seq;
  resend_after_done = act == LPWAN_BUSY_WAIT_RESEND;
  arm_deadline(after_ms);
}

static void do_send(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  const struct sensei_lpwan_config st = cfg_get();
  sensei_Send *s = &rq.send;

  if (atomic_get(&dfu_active)) {
    finish_send(-EAGAIN); /* on_up comes when the session ends */
    return;
  }
  if (lpwan_tech_of_radio(send_radio) == LPWAN_TECH_LTE && atomic_get(&gnss_pending)) {
    finish_send(-EAGAIN); /* a fix runs (a send that came before it was already asked for): on_up comes after it */
    return;
  }
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
  s->rai = send_radio == sensei_Radio_RADIO_LORA ? sensei_LteRai_LTE_RAI_OFF
                                                 : lpwan_rai_for_chunk((sensei_LteRai)st.lte_rai, send_last);
  send_rai = s->rai;
  s->payload.size = (pb_size_t)send_len;
  memcpy(s->payload.bytes, send_buf, send_len);
  idle_deadline = 0;
  rs.which_payload = 0;
  if (request(&rq, &rs, REQ_TO) != 0) {
    sensei_LpwanResult res = (sensei_LpwanResult)last_result;
    uint32_t next_max = rs.which_payload == sensei_LpwanResponse_send_tag ? rs.send.next_max_payload : 0;

    if (res == sensei_LpwanResult_LPWAN_INVALID) {
      /* The chunk does not fit the next uplink (MAC answers in FOpts): the answer says what fits. The app chunks
       * again (-EMSGSIZE) only if the size it will use is smaller; else the same chunk would be refused again. */
      lora_next_max = (uint8_t)MIN(next_max, UINT8_MAX);
      finish_send(send_radio == sensei_Radio_RADIO_LORA &&
                          lpwan_lora_chunk_too_big(st.lora_dr, st.lora_adr, next_max, send_len)
                      ? -EMSGSIZE
                      : -EIO);
    } else if (res == sensei_LpwanResult_LPWAN_BUSY && send_radio == sensei_Radio_RADIO_LORA) {
      busy_answer(&rs);
    } else if (res == sensei_LpwanResult_LPWAN_NOT_CONNECTED || res == sensei_LpwanResult_LPWAN_NOT_JOINED) {
      /* The shield restarted (or lost the connection) and request() started the state machine again: connect again and
       * let the app wait for on_up. A failed chunk would only be retried into the same refusal until the retries are
       * used up, and the message would wait for the next uplink time. -ENOTCONN and not -EAGAIN: this is not the duty
       * cycle. The new connection is a new socket for LTE, so the app must start the message again, and it counts
       * these (a shield that restarts on every send must not block the queue for ever). */
      finish_send(-ENOTCONN);
      (void)post(CMD_CONNECT, send_radio, false);
    } else {
      finish_send(-EIO);
    }
    return;
  }
  /* A send in LoRa can wait for the duty cycle of the LR1121 (minutes at DR0): the nRF9151 says how long. The
   * result may already be in (its tx_done came during the request). */
  if (!atomic_get(&send_finishing)) {
    uint32_t expected = rs.which_payload == sensei_LpwanResponse_send_tag ? rs.send.expected_ms : 0;

    arm_deadline(expected);
  }
}

static void do_eval(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  if (!link_up || sm.state != LPWAN_ST_CONNECTED || sm.tech != LPWAN_TECH_LTE) {
    eval_failed(false);
    return;
  }
  rq.which_body = sensei_LpwanRequest_lte_eval_tag;
  if (request(&rq, &rs, REQ_TO) != 0) {
    /* Refused (LPWAN_UNSUPPORTED: an older shield) or no answer: the result is not coming. */
    eval_failed(last_result == sensei_LpwanResult_LPWAN_UNSUPPORTED);
  }
}

/* Ask the shield for a fix. The result is an event (gnss_event); the deadline covers the time of the fix and the
 * margin. */
static void do_gnss(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  bool known_no;
  uint32_t timeout_s = 0;
  int err;

  gnss_open = true;
  if (atomic_get(&gnss_cancel_flag)) {
    gnss_finish(0, sensei_GnssFixStatus_GNSS_FIX_CANCELLED, NULL);
    return;
  }
  if (atomic_get(&dfu_active)) {
    gnss_finish(-EBUSY, sensei_GnssFixStatus_GNSS_FIX_BUSY, NULL); /* the update holds the link */
    return;
  }
  err = shield_up();
  if (err) {
    bool later = err == -EAGAIN || err == -EBUSY; /* a retry window of the bring-up, GAP9 on the bus */

    gnss_finish(later ? -EBUSY : err, later ? sensei_GnssFixStatus_GNSS_FIX_BUSY : sensei_GnssFixStatus_GNSS_FIX_FAILED,
                NULL);
    return;
  }
  k_spinlock_key_t k = k_spin_lock(&ver_lock);

  known_no = shield_ver_valid && !shield_ver.gnss;
  k_spin_unlock(&ver_lock, k);
  if (known_no) {
    gnss_finish(-ENOTSUP, sensei_GnssFixStatus_GNSS_FIX_NOT_SUPPORTED, NULL);
    return;
  }
  if (lpwan_gnss_admit(&sm, false) != sensei_LpwanResult_LPWAN_OK) {
    gnss_finish(-EBUSY, sensei_GnssFixStatus_GNSS_FIX_BUSY, NULL); /* LTE connects or sends */
    return;
  }
  (void)lpwan_gnss_timeout(gnss_timeout, &timeout_s);
  gnss_seq = lpwan_master_next_seq(&master); /* the result event may come before the answer is read */
  rq.which_body = sensei_LpwanRequest_gnss_fix_tag;
  rq.gnss_fix.timeout_s = gnss_timeout;
  err = request(&rq, &rs, REQ_TO);
  if (err) {
    switch (last_result) {
    case sensei_LpwanResult_LPWAN_UNSUPPORTED: /* an older shield app: it does not know the request */
      gnss_finish(-ENOTSUP, sensei_GnssFixStatus_GNSS_FIX_NOT_SUPPORTED, NULL);
      break;
    case sensei_LpwanResult_LPWAN_BUSY:
      gnss_finish(-EBUSY, sensei_GnssFixStatus_GNSS_FIX_BUSY, NULL);
      break;
    default:
      gnss_finish(err == -EBUSY ? -EBUSY : -EIO,
                  err == -EBUSY ? sensei_GnssFixStatus_GNSS_FIX_BUSY : sensei_GnssFixStatus_GNSS_FIX_FAILED, NULL);
      break;
    }
    return;
  }
  if (gnss_open) { /* not over already */
    gnss_deadline = k_uptime_get() + lpwan_gnss_wait_ms(timeout_s);
    idle_deadline = 0; /* the shield stays on until the result */
  }
}

static void do_gnss_cancel(void) {
  if (gnss_open && gnss_deadline != 0 && link_up) {
    (void)simple_request(sensei_LpwanRequest_gnss_cancel_tag); /* the event ends the fix */
  }
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
  if (!shield_on || atomic_get(&sending) || connecting || atomic_get(&dfu_active) || gnss_open) {
    return;
  }
  if (lpwan_upd_keep_power(&upd, k_uptime_get())) { /* a new shield image is not confirmed yet */
    idle_deadline = k_uptime_get() + LPWAN_UPD_POLL_MS;
    return;
  }
  if (lpwan_held_keeps_power(&held, (uint32_t)k_uptime_get())) {
    /* A send was given up, but the LR1121 may still hold its frame: the power would drop it. Look again later. */
    idle_deadline = k_uptime_get() + 10000;
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

/* ---- update of the shield ---- */

/* End the session: the link is free again, the shield may be gated off, the uplink queue tries again. */
static void dfu_session_end(const char *why) {
  if (!atomic_cas(&dfu_active, 1, 0)) {
    return;
  }
  LOG_INF("shield update session ends: %s", why);
  if (dfu_bus) {
    bus_release(BUS_DFU);
    dfu_bus = false;
  }
  dfu_modem_busy = false;
  idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
  up_radio = sensei_Radio_RADIO_BLE;
  submit_work(&flush_work);
}

/* Take the link for an update: no send or connect in progress, GAP9 kept off the bus, the shield on and idle. */
static int dfu_session_begin(void) {
  int err;

  if (atomic_get(&sending) || connecting) {
    return -EBUSY;
  }
  if (!bus_claim_wait(BUS_DFU, LPWAN_UPD_BUS_HOLD_MS, LPWAN_BUS_WAIT_MS)) {
    return -EBUSY; /* GAP9 owns the bus */
  }
  dfu_bus = true;
  atomic_set(&dfu_active, 1);
  dfu_start = dfu_last = dfu_asked = k_uptime_get();
  dfu_modem_busy = false;
  dfu_target = sensei_DfuTarget_DFU_TARGET_APP;
  idle_deadline = 0;
  err = shield_up();
  if (err == 0 && sm.state != LPWAN_ST_IDLE) { /* a radio is active: release it first */
    if (simple_request(sensei_LpwanRequest_lte_disconnect_tag) == 0) {
      (void)lpwan_master_fetch_events(&master, 8);
    }
    if (sm.state != LPWAN_ST_IDLE) {
      err = -EBUSY;
    }
  }
  if (err) {
    dfu_session_end("could not start");
  }
  return err;
}

/* When the thread must look at the session again (idle or total end, the next poll of a busy modem). */
static int64_t dfu_wake(void) {
  int64_t t = MIN(dfu_last + (int64_t)LPWAN_UPD_SESSION_IDLE_MS, dfu_start + (int64_t)LPWAN_UPD_SESSION_MAX_MS) + 1;

  if (dfu_modem_busy) {
    t = MIN(t, dfu_asked + (int64_t)LPWAN_UPD_POLL_MS);
  }
  return t;
}

static uint32_t dfu_timeout_ms(pb_size_t tag) {
  switch (tag) {
  case sensei_LpwanRequest_dfu_finish_tag:
    return 20000; /* reads the whole image back and hashes it */
  case sensei_LpwanRequest_dfu_status_tag:
  case sensei_LpwanRequest_dfu_confirm_tag:
    return REQ_TO + 1500;
  default:
    return 12000; /* a chunk may erase the padding of the image */
  }
}

/* One dfu_* request. The shield's own verdict (it refused, or our state machine did) is an answer, not an error. */
static int dfu_exchange(sensei_LpwanRequest *rq, sensei_LpwanResponse *rs) {
  int err;

  dfu_asked = k_uptime_get();
  master.ready_timeout_ms = DFU_READY_MS;
  err = request(rq, rs, dfu_timeout_ms(rq->which_body));
  master.ready_timeout_ms = 1000;
  if ((err == -EIO || err == -EPERM) && last_result >= 0) {
    err = 0;
    rs->result = (sensei_LpwanResult)last_result;
  }
  return err;
}

/* An answer of the shield arrived: the idle time and the lease start again, the session may be over, the modem may
 * need polling. */
static void dfu_answer(pb_size_t tag, const sensei_LpwanResponse *rs) {
  if (!atomic_get(&dfu_active)) {
    return;
  }
  dfu_last = k_uptime_get();
  (void)bus_claim(BUS_DFU, LPWAN_UPD_BUS_HOLD_MS); /* renew */
  if (rs->which_payload == sensei_LpwanResponse_dfu_tag) {
    dfu_target = rs->dfu.target;
  }
  dfu_modem_busy = lpwan_upd_modem_busy(&rs->dfu);
  if (tag == sensei_LpwanRequest_dfu_abort_tag || rs->dfu.state == sensei_DfuState_DFU_STATE_IDLE ||
      rs->dfu.state == sensei_DfuState_DFU_STATE_FAILED || rs->dfu.state == sensei_DfuState_DFU_STATE_DONE) {
    dfu_session_end("no transfer open");
  }
}

/* The host is gone (or slow) while the modem erases or updates: ask the shield, keep the session and the power until
 * it says DONE or FAILED. */
static void dfu_poll(void) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;

  memset(&rs, 0, sizeof(rs));
  rq.which_body = sensei_LpwanRequest_dfu_status_tag;
  if (dfu_exchange(&rq, &rs) == 0) {
    dfu_answer(rq.which_body, &rs);
  }
}

/* The shield marked its second slot for one test boot. The nRF5340 owns the power: cut it, wait, switch it on, and give
 * MCUboot the time to swap the images (tens of seconds, at the first start only). Then the new image answers the
 * handshake. If it speaks another protocol it is not confirmed, and the revert happens at the next power-up. */
static int dfu_restart_shield(struct dfu_job *j) {
  int err;

  LOG_INF("shield update: test swap requested, power-cycling the shield");
  lpwan_upd_apply_sent(&upd);
  shield_down();
  shield_retry_at = 0;
  proto_bad = false;
  err = shield_up_for(LPWAN_UPD_SWAP_TIMEOUT_MS);
  if (err == 0) {
    k_spinlock_key_t k = k_spin_lock(&ver_lock);

    j->res.version = shield_ver;
    j->res.has_version = true;
    k_spin_unlock(&ver_lock, k);
  } else {
    LOG_ERR(
        "shield update: the new image does not answer or is refused (%d); it is not confirmed and MCUboot goes back "
        "to the old one at the next power-up",
        err);
    proto_bad = false; /* the old image will be back: do not keep refusing the shield */
    shield_retry_at = 0;
  }
  return err;
}

/* The answer to dfu_apply of the app was lost: the shield may have marked the image. Ask it. If it did, the swap goes
 * on as if the answer had come; if it did not, the host repeats the apply. */
static int dfu_apply_lost(sensei_LpwanResponse *rs, int err) {
  if (err == -EBUSY || err == -ENOTCONN) {
    return err; /* the request did not go out */
  }
  for (int i = 0; i < 2; i++) {
    sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
    sensei_LpwanResponse st;

    memset(&st, 0, sizeof(st));
    rq.which_body = sensei_LpwanRequest_dfu_status_tag;
    if (dfu_exchange(&rq, &st) == 0 && st.which_payload == sensei_LpwanResponse_dfu_tag) {
      if (lpwan_upd_apply_took_effect(&st.dfu)) {
        LOG_WRN("dfu_apply: no answer, but the shield marked the image");
        *rs = st;
        rs->result = sensei_LpwanResult_LPWAN_OK;
        return 0;
      }
      break;
    }
  }
  return err;
}

static void do_dfu(void) {
  struct dfu_job *j = &dfu_job;
  sensei_LpwanResponse rs;
  pb_size_t tag = j->req.which_body;
  bool was_on = shield_on;
  int err;

  memset(&j->res, 0, sizeof(j->res));
  memset(&rs, 0, sizeof(rs));
  if (lpwan_upd_cooling(dfu_cooldown_until, k_uptime_get()) && tag != sensei_LpwanRequest_dfu_status_tag &&
      tag != sensei_LpwanRequest_dfu_confirm_tag && tag != sensei_LpwanRequest_dfu_abort_tag) {
    err = -EBUSY; /* the last session was too long: uplinks and GAP9 first */
  } else if (tag == sensei_LpwanRequest_dfu_begin_tag && !atomic_get(&dfu_active)) {
    err = dfu_session_begin();
  } else {
    err = shield_up();
  }
  if (err == 0 && tag == sensei_LpwanRequest_dfu_begin_tag) {
    uint32_t bit = j->req.dfu_begin.target == sensei_DfuTarget_DFU_TARGET_MODEM ? LPWAN_DFU_TARGET_MODEM_BIT
                                                                                : LPWAN_DFU_TARGET_APP_BIT;
    k_spinlock_key_t k = k_spin_lock(&ver_lock);
    bool no = shield_ver_valid && (shield_ver.dfu_targets & bit) == 0;

    k_spin_unlock(&ver_lock, k);
    if (no) {
      err = -ENOTSUP; /* an older shield app, or the target is not built in */
      dfu_session_end("no update support");
    }
  }
  if (err == 0) {
    if (tag == sensei_LpwanRequest_dfu_apply_tag && dfu_target == sensei_DfuTarget_DFU_TARGET_APP) {
      lpwan_upd_apply_sent(&upd); /* the answer may be lost: a power-up after this may be a swap */
    }
    err = dfu_exchange(&j->req, &rs);
    if (err != 0 && tag == sensei_LpwanRequest_dfu_apply_tag) {
      err = dfu_apply_lost(&rs, err);
    }
  }
  if (err == 0) {
    j->res.result = rs.result;
    if (rs.which_payload == sensei_LpwanResponse_dfu_tag) {
      j->res.status = rs.dfu;
    }
    if (rs.result == sensei_LpwanResult_LPWAN_OK && tag == sensei_LpwanRequest_dfu_apply_tag &&
        rs.dfu.target == sensei_DfuTarget_DFU_TARGET_APP) {
      err = dfu_restart_shield(j);
      dfu_session_end(err ? "restart failed" : "restarted");
    } else {
      dfu_answer(tag, &rs);
      if (tag == sensei_LpwanRequest_dfu_status_tag || tag == sensei_LpwanRequest_dfu_confirm_tag) {
        sensei_LpwanVersion v;

        if (read_version(&v) == 0) { /* the version goes with the status; an image on trial is looked after */
          j->res.version = v;
          j->res.has_version = true;
          trial_step(&v);
        }
      }
    }
  }
  if (!was_on && shield_on && !atomic_get(&dfu_active) && !connecting && !atomic_get(&sending)) {
    idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL; /* a status call must not keep it on */
  }
  j->err = err;
  k_work_submit_to_queue(hooks.wq != NULL ? hooks.wq : &k_sys_work_q, &j->work);
}

static void dfu_done_fn(struct k_work *w) {
  struct dfu_job *j = CONTAINER_OF(w, struct dfu_job, work);
  sensei_lpwan_dfu_done_fn cb = j->done;

  if (cb != NULL) {
    cb(j->err, &j->res, j->user);
  }
  atomic_set(&dfu_busy, 0);
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
      int err = shield_up();

      if (err == 0) {
        err = read_version(&version_out);
      }
      version_err = err;
      atomic_set(&version_wanted, 0);
      k_sem_give(&version_sem);
      if (!was_on && shield_on && !connecting) {
        idle_deadline = k_uptime_get() + CONFIG_SENSEI_LPWAN_IDLE_S * 1000LL;
      }
    }
    break;
  case CMD_DFU:
    do_dfu();
    break;
  case CMD_EVAL:
    do_eval();
    break;
  case CMD_GNSS:
    do_gnss();
    break;
  case CMD_GNSS_CANCEL:
    do_gnss_cancel();
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

/* The send deadline passed. A LoRa frame may still wait in the LR1121 (the duty cycle, a slow estimate): ask the
 * nRF9151. Returns true if the send is settled or waits on (nothing more to do now). */
static bool deadline_passed(int64_t now) {
  sensei_LpwanRequest rq = sensei_LpwanRequest_init_zero;
  sensei_LpwanResponse rs;
  bool known;

  if (send_radio != sensei_Radio_RADIO_LORA) {
    return false;
  }
  rq.which_body = sensei_LpwanRequest_get_status_tag;
  known = request(&rq, &rs, REQ_TO) == 0 && rs.which_payload == sensei_LpwanResponse_status_tag;
  switch (lpwan_held_deadline(send_wait_seq, deadline_extended, known, &rs.status)) {
  case LPWAN_DL_OK:
  case LPWAN_DL_FAILED: {
    bool ok = rs.status.late_tx_ok;

    LOG_INF("tx result of seq %u from the status: %s", (unsigned)send_wait_seq, ok ? "ok" : "failed");
    lpwan_held_result(&held, send_wait_seq);
    lpwan_sm_abort_send(&sm);
    resend_after_done = false;
    finish_send(ok ? 0 : -EIO);
    return true;
  }
  case LPWAN_DL_EXTEND:
    LOG_WRN("the LR1121 still holds the frame of seq %u: waiting longer", (unsigned)send_wait_seq);
    deadline_extended = true;
    send_deadline = now + (send_deadline - send_start);
    return true;
  default:
    return false;
  }
}

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
  if (atomic_get(&sending) && send_deadline && now > send_deadline && !deadline_passed(now)) {
    LOG_ERR("no transmit result");
    lpwan_sm_abort_send(&sm); /* the late tx_done, if it comes, is ignored: its seq is no longer sent */
    finish_send(-ETIMEDOUT);
    if (++tx_timeouts >= CONFIG_SENSEI_LPWAN_LINK_FAILURES) {
      link_recover("no transmit result");
    }
  }
  if (gnss_open && gnss_deadline != 0 && now > gnss_deadline) {
    LOG_ERR("no GNSS result");
    do_gnss_cancel(); /* the shield may still run it */
    gnss_finish(-ETIMEDOUT, sensei_GnssFixStatus_GNSS_FIX_FAILED, NULL);
  }
  if (idle_deadline && now > idle_deadline) {
    idle_action();
  }
  if (atomic_get(&dfu_active)) {
    switch (lpwan_upd_session_check(dfu_start, dfu_last, now)) {
    case LPWAN_UPD_END_IDLE:
      dfu_session_end("no request for a while"); /* the shield keeps its transfer: begin with resume continues it */
      break;
    case LPWAN_UPD_END_TOTAL:
      dfu_session_end("session too long");
      dfu_cooldown_until = now + LPWAN_UPD_COOLDOWN_MS;
      break;
    default:
      if (lpwan_upd_poll_due(dfu_modem_busy, dfu_asked, now)) {
        dfu_poll();
      }
      break;
    }
  }
  if (lpwan_upd_confirm_due(&upd) && shield_on && link_up && !booting && now >= trial_poll_at) {
    trial_poll(now);
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
    if (gnss_open && gnss_deadline != 0 && gnss_deadline < next) {
      next = gnss_deadline;
    }
    if (atomic_get(&dfu_active) && dfu_wake() < next) {
      next = dfu_wake();
    }
    if (lpwan_upd_confirm_due(&upd) && shield_on && trial_poll_at < next) {
      next = trial_poll_at;
    }
    k_timeout_t to = next == INT64_MAX ? K_FOREVER : K_MSEC(MAX(next - now, 1));

    (void)k_poll(ev, 2, to);

    if (atomic_cas(&gap9_bus_changed, 1, 0)) {
      level_update(); /* GAP9 took or gave back the bus: the level shifter follows */
    }
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

#ifdef CONFIG_SENSEI_LPWAN_SIM
static void event_isr_sim(void) { k_sem_give(&event_sem); }
#else
static void event_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
  ARG_UNUSED(port);
  ARG_UNUSED(cb);
  ARG_UNUSED(pins);
  k_sem_give(&event_sem);
}
#endif

static int post(enum cmd_type type, sensei_Radio radio, bool on) {
  struct cmd c = {.type = type, .radio = radio, .on = on};

  return k_msgq_put(&cmd_q, &c, K_NO_WAIT) == 0 ? 0 : -EBUSY;
}

/* ---- radios ---- */

uint32_t sensei_lpwan_lora_retry_after_ms(void) {
  int64_t left = lora_retry_at - k_uptime_get();

  return lora_retry_at != 0 && left > 0 ? (uint32_t)MIN(left, (int64_t)UINT32_MAX) : 0u;
}

bool sensei_lpwan_radio_ready(sensei_Radio r) {
  lpwan_tech_t want = lpwan_tech_of_radio(r);

  if (atomic_get(&dfu_active)) {
    return false; /* an update of the shield holds the link: on_up comes when it ends */
  }

  if (want == LPWAN_TECH_LTE && atomic_get(&gnss_pending)) {
    return false; /* a fix runs: LTE stays out of the way, on_up(RADIO_BLE) comes after it */
  }
  if (r == sensei_Radio_RADIO_LORA && sensei_lpwan_lora_retry_after_ms() != 0) {
    return false; /* refused for the duty cycle: do not wake the shield, on_up comes when it may be tried again */
  }
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

uint32_t sensei_lpwan_lora_mtu(void) {
  const struct sensei_lpwan_config st = cfg_get();

  return lpwan_lora_chunk_max(st.lora_dr, st.lora_adr, lora_next_max);
}

int sensei_lpwan_radio_send(sensei_Radio r, const uint8_t *chunk, size_t len, void (*done)(int, void *), void *user) {
  return sensei_lpwan_radio_send_ex(r, chunk, len, false, done, user);
}

int sensei_lpwan_radio_send_ex(sensei_Radio r, const uint8_t *chunk, size_t len, bool last, void (*done)(int, void *),
                               void *user) {
  if (len == 0 || len > sizeof(send_buf)) {
    return -EINVAL;
  }
  if (atomic_get(&dfu_active)) {
    return -EAGAIN; /* the app waits for on_up (RADIO_BLE) at the end of the session: no failure */
  }
  if (lpwan_tech_of_radio(r) == LPWAN_TECH_LTE && atomic_get(&gnss_pending)) {
    return -EAGAIN; /* a fix runs: on_up (RADIO_BLE) comes when it is over, no failure */
  }
  if (r == sensei_Radio_RADIO_LORA) {
    const struct sensei_lpwan_config st = cfg_get();

    if (len > lpwan_lora_max_payload(st.lora_dr, st.lora_adr)) {
      /* The data rate was lowered after the message was chunked: chunk it again (the nRF9151 checks what the LR1121
       * takes now and answers -EMSGSIZE the same way). */
      return -EMSGSIZE;
    }
    if (sensei_lpwan_lora_retry_after_ms() != 0) {
      return -EAGAIN; /* the shield refused a send for the duty cycle; on_up comes when it may be tried again */
    }
  }
  if (!atomic_cas(&sending, 0, 1)) {
    return -EBUSY;
  }
  memcpy(send_buf, chunk, len);
  send_len = len;
  send_radio = r;
  send_last = last;
  send_unconfirmed = false;
  send_attempts = 0;
  send_done = done;
  send_user = user;
  if (post(CMD_SEND, r, false) != 0) {
    atomic_set(&sending, 0);
    return -EBUSY;
  }
  return 0;
}

/* ---- API ---- */

bool sensei_lpwan_send_unconfirmed(void) { return send_unconfirmed; }

uint32_t sensei_lpwan_send_attempts(void) { return send_attempts; }

uint32_t sensei_lpwan_lte_no_answer(void) { return lte_noack; }

enum sensei_lpwan_link sensei_lpwan_lte_link_check(void) {
#ifdef CONFIG_SENSEI_LPWAN_LTE_EVAL
  enum lpwan_eval_action act;
  k_spinlock_key_t k = k_spin_lock(&eval_lock);
  uint32_t now = (uint32_t)k_uptime_get();

  act = lpwan_eval_decide(&eval, now, CONFIG_SENSEI_LPWAN_EVAL_VALID_S * 1000u, EVAL_TIMEOUT_MS,
                          CONFIG_SENSEI_LPWAN_EVAL_RETRY_S * 1000u);
  if (act == LPWAN_EVAL_ASK) {
    eval.pending = true;
    eval.asked = now;
  }
  k_spin_unlock(&eval_lock, k);
  switch (act) {
  case LPWAN_EVAL_BAD:
    return SENSEI_LPWAN_LINK_BAD;
  case LPWAN_EVAL_WAIT:
    return SENSEI_LPWAN_LINK_WAIT;
  case LPWAN_EVAL_ASK:
    if (post(CMD_EVAL, sensei_Radio_RADIO_BLE, false) == 0) {
      return SENSEI_LPWAN_LINK_WAIT;
    }
    k = k_spin_lock(&eval_lock);
    eval.pending = false; /* the queue is full: send as usual */
    k_spin_unlock(&eval_lock, k);
    return SENSEI_LPWAN_LINK_OK;
  default:
    return SENSEI_LPWAN_LINK_OK;
  }
#else
  return SENSEI_LPWAN_LINK_OK;
#endif
}

int sensei_lpwan_lte_info(struct sensei_lpwan_lte_info *out) {
  k_spinlock_key_t k = k_spin_lock(&eval_lock);
  bool ok = eval_info_valid;

  if (ok) {
    *out = eval_info;
    out->rai = rai_seen;
    out->age_s = (uint32_t)((k_uptime_get() - eval_info_at) / 1000);
  }
  k_spin_unlock(&eval_lock, k);
  return ok ? 0 : -ENODATA;
}

void sensei_lpwan_set_config(const struct sensei_lpwan_config *c) {
  k_spinlock_key_t k = k_spin_lock(&cfg_lock);

  cfg = *c;
  k_spin_unlock(&cfg_lock, k);
  lora_next_max = 0; /* the data rate may have changed */
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
  k_work_init(&dfu_job.work, dfu_done_fn);
  k_work_init(&gnss_work, gnss_done_fn);
  k_work_init_delayable(&retry_work, retry_fn);
  k_work_init(&time_item.work, time_work);
  lpwan_sm_init(&sm);
  lpwan_upd_init(&upd); /* the node may have cut the power in the middle of a swap */
  lpwan_master_init(&master, &mops, NULL);
  master.ready_timeout_ms = 1000;

#ifdef CONFIG_SENSEI_LPWAN_SIM
  err = lpwan_sim_master_init(event_isr_sim);
  if (err) {
    LOG_ERR("simulated shield link failed: %d", err);
    return err;
  }
  shield_off_at = k_uptime_get();
  bus_set_listener(gap9_listener, NULL);
#else
  if (!device_is_ready(spi_dev) || !gpio_is_ready_dt(&pin_event) || !gpio_is_ready_dt(&pin_busy) ||
      !gpio_is_ready_dt(&pin_power) || !gpio_is_ready_dt(&pin_gap_sel)) {
    LOG_ERR("LPWAN shield devices not ready (is LPWAN_Shield_v1 in the build?)");
    return -ENODEV;
  }
  /* Shield off, SPI pins parked, GAP9 free to use the bus. */
  err |= gpio_pin_configure_dt(&pin_gap_sel, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_event, GPIO_INPUT | GPIO_PULL_DOWN);
  err |= expander_outputs(false);
  shield_off_at = k_uptime_get(); /* the shield may have been on before a reset of the nRF5340 */
  pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPI_BUS), PINCTRL_STATE_SLEEP);
  (void)gpio_pin_configure_dt(&spi_nrf91.config.cs.gpio, GPIO_DISCONNECTED);
  if (err) {
    LOG_ERR("shield pins failed (expander at 0x20 not answering?)");
    return -EIO;
  }
  bus_set_listener(gap9_listener, NULL);
  gpio_init_callback(&event_cb, event_isr, BIT(pin_event.pin));
  err = gpio_add_callback(pin_event.port, &event_cb);
  err |= gpio_pin_interrupt_configure_dt(&pin_event, GPIO_INT_EDGE_RISING);
  if (err) {
    return -EIO;
  }
#endif

  k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), thread_fn, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_SENSEI_LPWAN_THREAD_PRIORITY), 0, K_NO_WAIT);
  k_thread_name_set(&thread, "lpwan");
  LOG_INF("LPWAN client ready, shield off");
  return 0;
}

/* ---- GNSS: API ---- */

int sensei_lpwan_gnss_fix(uint32_t timeout_s, sensei_lpwan_gnss_done_fn done, void *user) {
  uint32_t t;

  if (!lpwan_gnss_timeout(timeout_s, &t)) {
    return -EINVAL;
  }
  if (!atomic_cas(&gnss_pending, 0, 1)) {
    return -EBUSY;
  }
  gnss_timeout = timeout_s;
  gnss_done = done;
  gnss_user = user;
  atomic_set(&gnss_cancel_flag, 0);
  if (post(CMD_GNSS, sensei_Radio_RADIO_BLE, false) != 0) {
    atomic_set(&gnss_pending, 0);
    return -EBUSY;
  }
  return 0;
}

int sensei_lpwan_gnss_cancel(void) {
  if (!atomic_get(&gnss_pending)) {
    return 0;
  }
  atomic_set(&gnss_cancel_flag, 1);
  return post(CMD_GNSS_CANCEL, sensei_Radio_RADIO_BLE, false);
}

bool sensei_lpwan_gnss_busy(void) { return atomic_get(&gnss_pending) != 0; }

int sensei_lpwan_gnss_supported(void) {
  k_spinlock_key_t k = k_spin_lock(&ver_lock);
  int r = shield_ver_valid ? (shield_ver.gnss ? 1 : 0) : -ENODATA;

  k_spin_unlock(&ver_lock, k);
  return r;
}

/* ---- update of the shield: API ---- */

int sensei_lpwan_dfu_submit(const sensei_LpwanRequest *req, sensei_lpwan_dfu_done_fn done, void *user) {
  if (req->which_body < sensei_LpwanRequest_dfu_begin_tag || req->which_body > sensei_LpwanRequest_dfu_confirm_tag) {
    return -EINVAL;
  }
  if (!atomic_cas(&dfu_busy, 0, 1)) {
    return -EBUSY;
  }
  dfu_job.req = *req;
  dfu_job.done = done;
  dfu_job.user = user;
  if (post(CMD_DFU, sensei_Radio_RADIO_BLE, false) != 0) {
    atomic_set(&dfu_busy, 0);
    return -EBUSY;
  }
  return 0;
}

bool sensei_lpwan_dfu_active(void) { return atomic_get(&dfu_active) != 0; }

int sensei_lpwan_shield_diag(struct sensei_lpwan_shield_diag *out) {
  k_spinlock_key_t k = k_spin_lock(&diag_lock);
  bool ok = shield_diag_valid;

  if (ok) {
    *out = shield_diag;
  }
  k_spin_unlock(&diag_lock, k);
  return ok ? 0 : -ENODATA;
}

int sensei_lpwan_shield_version(sensei_LpwanVersion *out) {
  k_spinlock_key_t k = k_spin_lock(&ver_lock);
  bool ok = shield_ver_valid;

  if (ok) {
    *out = shield_ver;
  }
  k_spin_unlock(&ver_lock, k);
  return ok ? 0 : -ENODATA;
}

int sensei_lpwan_lora_keys_queue(const sensei_LoraProvision *k) {
  if (k == NULL) {
    return -EINVAL;
  }
  keys = *k;
  keys_tries = 0;
  atomic_set(&keys_pending, 1);
  return 0;
}

int sensei_lpwan_lora_provision(const sensei_LoraProvision *k) {
  int err = sensei_lpwan_lora_keys_queue(k);

  return err ? err : post(CMD_PROVISION, sensei_Radio_RADIO_LORA, false);
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
  if (version_err == 0) {
    *out = version_out;
  }
  return version_err; /* -EPROTONOSUPPORT: the shield speaks another protocol version */
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
  i->proto_mismatch = proto_bad;
  i->recoveries = recoveries;
  i->failures = master.failures;
  i->lte_acked = lte_acked;
  i->lte_noack = lte_noack;
  strncpy(i->fw, fw_str, sizeof(i->fw) - 1);
  strncpy(i->modem, modem_str, sizeof(i->modem) - 1);
}

const char *sensei_lpwan_state_str(void) {
  if (proto_bad) {
    return "protocol mismatch";
  }
  if (!shield_on) {
    return "shield off";
  }
  return lpwan_state_name(sm.state);
}
