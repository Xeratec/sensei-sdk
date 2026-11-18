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
 * @file lte.c
 * @brief LTE-M / NB-IoT backend (nRF modem library, lte_lc, BSD sockets).
 *
 * Three paths, as in the commissioning project: connect (one worker thread that also runs sends
 * and disconnects), receive (own thread, blocks in poll) and the event callback of lte_lc. None of
 * them is the SPI link thread.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <date_time.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "lte.h"
#include "sensei_lpwan/lpwan_counters.h"
#include "sensei_lpwan/lpwan_rai.h"
#include "sensei_lpwan/lpwan_timers.h"
#include "service.h"

LOG_MODULE_REGISTER(lte, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

enum cmd { CMD_CONNECT, CMD_DISCONNECT, CMD_SEND, CMD_TIME, CMD_SLEEP_DEEP, CMD_EVAL };

struct msg {
  enum cmd cmd;
  uint32_t seq;
  uint16_t len;
  sensei_LteRai rai; /* CMD_SEND: the flag of the client for this chunk (last chunk of a message) */
  uint8_t data[1024];
};

K_MSGQ_DEFINE(lte_q, sizeof(struct msg), 2, 4);
static K_SEM_DEFINE(registered_sem, 0, 1);
static K_SEM_DEFINE(time_sem, 0, 1);
static K_MUTEX_DEFINE(cfg_lock);

static sensei_LteConfigure cfg = {
    .mode = sensei_LteMode_LTE_M,
    .psm_tau_s = 0,
    .psm_active_s = 0,
    .server_port = 0,
    .udp = true,
};
static bool configured;
static bool modem_ready;
static K_MUTEX_DEFINE(modem_lock);
static char modem_ver[25]; /* the modem firmware version once the modem library has started, else empty */
static volatile bool registered;
static volatile bool want_connected;
static volatile bool connect_cancelled; /* lte_disconnect() asked the connect in progress to stop */
static int sock = -1;
static int32_t last_rsrp_dbm;
static bool last_rsrp_valid; /* the modem gave a valid RSRP at the last look; else the status says "no value" */

/* The answer of the server to the last chunk (LTE_RAI_ONE_RESP): the receive thread gives the semaphore, the worker
 * waits for it. wait_key is lpwan_answer_key() of the chunk that waits, 0: none. */
static K_SEM_DEFINE(answer_sem, 0, 1);
static atomic_t wait_key;
static atomic_t n_answers, n_no_answer; /* answered / sent but unconfirmed (TxDone.unconfirmed) */
/* RRC connected time, attaches and PSM entries (LpwanStatus): fed by the lte_lc handler, read by the service thread */
static struct lpwan_counters cnt;
static struct k_spinlock cnt_lock;
static volatile sensei_RaiSupport rai_support;

static struct k_thread lte_thread, rx_thread;
K_THREAD_STACK_DEFINE(lte_stack, CONFIG_LPWAN_SHIELD_LTE_STACK);
K_THREAD_STACK_DEFINE(rx_stack, 2048);

static sensei_Radio radio_id(void) {
  return cfg.mode == sensei_LteMode_LTE_NB_IOT ? sensei_Radio_RADIO_NBIOT : sensei_Radio_RADIO_LTEM;
}

/* ---- lte_lc and date_time callbacks (system work queue context) ---- */

static void lte_handler(const struct lte_lc_evt *const evt) {
  switch (evt->type) {
  case LTE_LC_EVT_NW_REG_STATUS: {
    bool was_registered = registered;

    registered =
        evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME || evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;
    k_spinlock_key_t k = k_spin_lock(&cnt_lock);

    lpwan_cnt_reg(&cnt, registered, k_uptime_get_32());
    k_spin_unlock(&cnt_lock, k);
    LOG_INF("registration status %d", (int)evt->nw_reg_status);
    if (registered) {
      k_sem_give(&registered_sem);
    } else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTRATION_DENIED) {
      svc_emit_error(sensei_LpwanResult_LPWAN_NO_NETWORK, "registration denied");
    } else if (was_registered && want_connected && sock >= 0) {
      /* Coverage lost while connected: the modem searches again by itself. Tell the master, which sees
       * failed sends meanwhile (worker_send fails them while not registered). */
      svc_emit_error(sensei_LpwanResult_LPWAN_NO_NETWORK, "registration lost");
    }
    break;
  }
  case LTE_LC_EVT_PSM_UPDATE:
    LOG_INF("PSM: TAU %d s, active %d s", evt->psm_cfg.tau, evt->psm_cfg.active_time);
    if (evt->psm_cfg.tau < 0 && cfg.psm_tau_s > 0) {
      LOG_WRN("the network grants no PSM%s", IS_ENABLED(CONFIG_LTE_PROPRIETARY_PSM_REQ) ? ": proprietary PSM" : "");
    }
    break;
#ifdef CONFIG_LTE_LC_RAI_MODULE
  case LTE_LC_EVT_RAI_UPDATE:
    rai_support = lpwan_rai_support(evt->rai_cfg.as_rai, evt->rai_cfg.cp_rai);
    LOG_INF("RAI of the cell: AS %d, CP %d", (int)evt->rai_cfg.as_rai, (int)evt->rai_cfg.cp_rai);
    break;
#endif
  case LTE_LC_EVT_EDRX_UPDATE:
    LOG_INF("eDRX: cycle %d, ptw %d", (int)evt->edrx_cfg.edrx, (int)evt->edrx_cfg.ptw);
    break;
  case LTE_LC_EVT_RRC_UPDATE: {
    k_spinlock_key_t k = k_spin_lock(&cnt_lock);

    lpwan_cnt_rrc(&cnt, evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED, k_uptime_get_32());
    k_spin_unlock(&cnt_lock, k);
    LOG_DBG("RRC %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
    break;
  }
#ifdef CONFIG_LTE_LC_MODEM_SLEEP_MODULE
  case LTE_LC_EVT_MODEM_SLEEP_ENTER:
    if (evt->modem_sleep.type == LTE_LC_MODEM_SLEEP_PSM ||
        evt->modem_sleep.type == LTE_LC_MODEM_SLEEP_PROPRIETARY_PSM) {
      k_spinlock_key_t k = k_spin_lock(&cnt_lock);

      lpwan_cnt_psm(&cnt);
      k_spin_unlock(&cnt_lock, k);
    }
    break;
#endif
  default:
    break;
  }
}

static void date_time_handler(const struct date_time_evt *evt) {
  if (evt->type == DATE_TIME_OBTAINED_MODEM || evt->type == DATE_TIME_OBTAINED_NTP ||
      evt->type == DATE_TIME_OBTAINED_EXT) {
    k_sem_give(&time_sem);
  }
}

/* ---- helpers ---- */

static int read_unix_time(uint32_t *out) {
  int64_t ms;

  if (date_time_now(&ms) != 0) {
    return -ENODATA;
  }
  *out = (uint32_t)(ms / 1000);
  return 0;
}

static void emit_time(void) {
  uint32_t t;

  k_sem_reset(&time_sem);
  if (read_unix_time(&t) != 0) {
    date_time_update_async(date_time_handler);
    if (k_sem_take(&time_sem, K_SECONDS(CONFIG_LPWAN_SHIELD_LTE_TIME_WAIT_S)) != 0 || read_unix_time(&t) != 0) {
      LOG_WRN("no network time");
      return;
    }
  }
  svc_emit_time(radio_id(), t);
}

static void close_socket(void) {
  if (sock >= 0) {
    (void)zsock_close(sock);
    sock = -1;
  }
}

static int open_socket(void) {
  struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = cfg.udp ? SOCK_DGRAM : SOCK_STREAM};
  struct zsock_addrinfo *res = NULL;
  char port[12];
  int fd, err;

  if (cfg.server_host[0] == '\0' || cfg.server_port == 0) {
    return -EINVAL;
  }
  snprintf(port, sizeof(port), "%u", (unsigned)cfg.server_port);
  err = zsock_getaddrinfo(cfg.server_host, port, &hints, &res);
  if (err || res == NULL) {
    LOG_ERR("getaddrinfo failed: %d", err);
    return -EHOSTUNREACH;
  }
  fd = zsock_socket(res->ai_family, res->ai_socktype, cfg.udp ? IPPROTO_UDP : IPPROTO_TCP);
  if (fd < 0) {
    zsock_freeaddrinfo(res);
    return -errno;
  }
  if (zsock_connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    err = -errno;
    zsock_close(fd);
    zsock_freeaddrinfo(res);
    return err;
  }
  zsock_freeaddrinfo(res);
  close_socket();
  sock = fd;
  return 0;
}

static void apply_power_saving(void) {
  char tau[9], act[9], cyc[5];
  bool nb = cfg.mode == sensei_LteMode_LTE_NB_IOT;
  enum lte_lc_lte_mode lm = nb ? LTE_LC_LTE_MODE_NBIOT : LTE_LC_LTE_MODE_LTEM;

  if (cfg.psm_tau_s > 0 && lpwan_encode_tau(cfg.psm_tau_s, tau) == 0 &&
      lpwan_encode_active(cfg.psm_active_s, act) == 0) {
    if (lte_lc_psm_param_set(tau, act) == 0 && lte_lc_psm_req(true) == 0) {
      LOG_INF("PSM requested: TAU %s, active %s", tau, act);
    }
  } else {
    (void)lte_lc_psm_req(false);
  }
  if (cfg.edrx_cycle_ms > 0 && lpwan_encode_edrx(cfg.edrx_cycle_ms, nb, cyc) == 0) {
    char ptw[5];

    (void)lte_lc_edrx_param_set(lm, cyc);
    if (cfg.edrx_ptw_ms > 0 && lpwan_encode_ptw(cfg.edrx_ptw_ms, nb, ptw) == 0) {
      (void)lte_lc_ptw_set(lm, ptw);
    }
    if (lte_lc_edrx_req(true) == 0) {
      LOG_INF("eDRX requested: cycle %s", cyc);
    }
  } else {
    (void)lte_lc_edrx_req(false);
  }
}

/* The modem firmware version (for LpwanVersion.modem), asked once the modem library runs. */
static void read_modem_version(void) {
  char v[sizeof(modem_ver)];

  if (nrf_modem_at_scanf("AT%SHORTSWVER", "%%SHORTSWVER: %24s", v) == 1) {
    snprintf(modem_ver, sizeof(modem_ver), "%s", v);
  }
}

int lte_modem_init(void) {
  int err = 0;

  k_mutex_lock(&modem_lock, K_FOREVER);
  if (!modem_ready) {
    err = nrf_modem_lib_init();
    if (err) {
      LOG_ERR("nrf_modem_lib_init: %d", err);
      err = -EIO;
    } else {
      lte_lc_register_handler(lte_handler);
      modem_ready = true;
      read_modem_version();
    }
  }
  k_mutex_unlock(&modem_lock);
  return err;
}

void lte_modem_restarted(bool ok) {
  k_mutex_lock(&modem_lock, K_FOREVER);
  modem_ready = ok; /* the delta update shut the library down and started it again; if that failed, the next
                       connect starts it */
  if (ok) {
    read_modem_version();
  }
  k_mutex_unlock(&modem_lock);
}

static int do_connect(void) {
  int err = lte_modem_init();

  if (err) {
    return err;
  }
  err =
      lte_lc_system_mode_set(cfg.mode == sensei_LteMode_LTE_NB_IOT ? LTE_LC_SYSTEM_MODE_NBIOT : LTE_LC_SYSTEM_MODE_LTEM,
                             LTE_LC_SYSTEM_MODE_PREFER_AUTO);
  if (err) {
    LOG_ERR("system mode: %d", err);
    return -EIO;
  }
  if (cfg.apn[0] != '\0') {
    (void)nrf_modem_at_printf("AT+CGDCONT=0,\"IP\",\"%s\"", cfg.apn);
  }
  apply_power_saving();

  /* A disconnect that came while this connect was queued or in the modem setup: the flag was set by it, the semaphore
   * reset below would eat its give. */
  k_sem_reset(&registered_sem);
  if (!want_connected || connect_cancelled) {
    return -ECANCELED;
  }
  err = lte_lc_connect_async(lte_handler);
  if (err) {
    LOG_ERR("connect_async: %d", err);
    return -EIO;
  }
  if (k_sem_take(&registered_sem, K_SECONDS(CONFIG_LPWAN_SHIELD_LTE_CONNECT_TIMEOUT_S)) != 0) {
    (void)lte_lc_offline();
    return -ETIMEDOUT;
  }
  /* The semaphore is also given by lte_disconnect(): registered is what counts. */
  if (connect_cancelled || !want_connected) {
    return -ECANCELED;
  }
  return registered ? 0 : -ENETDOWN;
}

static void worker_connect(void) {
  int err = do_connect();

  if (err == 0 && want_connected) {
    err = open_socket();
    if (err) {
      LOG_ERR("socket: %d", err);
      svc_emit_error(sensei_LpwanResult_LPWAN_NO_NETWORK, "server unreachable");
      return;
    }
    svc_emit_radio(sensei_LpwanEvent_connected_tag, radio_id());
    emit_time();
  } else if (err && err != -ECANCELED) { /* cancelled: the disconnect that did it reports itself */
    svc_emit_error(err == -ETIMEDOUT ? sensei_LpwanResult_LPWAN_TIMEOUT : sensei_LpwanResult_LPWAN_NO_NETWORK,
                   err == -ETIMEDOUT ? "no network" : "modem error");
  }
}

/* ---- send with RAI (the sequence is in lib/sensei_lpwan/src/lpwan_rai.c, host tested) ---- */

#ifdef CONFIG_LPWAN_SHIELD_SIM
int sim_modem_set_rai(int opt); /* sim/sim_modem.c: a host socket has no RAI option */
#endif

static int io_set_rai(void *ctx, enum lpwan_rai_opt opt) {
  int v = (int)opt;

  ARG_UNUSED(ctx);
#ifdef CONFIG_LPWAN_SHIELD_SIM
  return sim_modem_set_rai(v);
#else
  return zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &v, sizeof(v)) == 0 ? 0 : -errno;
#endif
}

static int io_send(void *ctx, const uint8_t *data, size_t len) {
  ARG_UNUSED(ctx);
  return zsock_send(sock, data, len, 0) == (ssize_t)len ? 0 : -EIO;
}

/* The wait ends early when the coverage is lost or the node asked for a disconnect: no answer can come then. */
#define WAIT_SLICE_MS 1000U

static bool io_wait_answer(void *ctx, uint32_t timeout_ms) {
  ARG_UNUSED(ctx);
  while (timeout_ms > 0) {
    uint32_t slice = MIN(timeout_ms, WAIT_SLICE_MS);

    if (k_sem_take(&answer_sem, K_MSEC(slice)) == 0) {
      return true;
    }
    timeout_ms -= slice;
    if (!registered || !want_connected) {
      break;
    }
  }
  return false;
}

/* Time the server gets to answer, from the send(): the modem sets the RRC connection up first (about 3 s on LTE-M,
 * more with the repetitions of NB-IoT at coverage level 1 and 2: 10 to 30 s for 1 kB). So it depends on the radio. */
static uint32_t answer_wait_ms(void) {
#ifdef CONFIG_LPWAN_SHIELD_RAI
  return (cfg.mode == sensei_LteMode_LTE_NB_IOT ? CONFIG_LPWAN_SHIELD_LTE_ANSWER_WAIT_NBIOT_S
                                                : CONFIG_LPWAN_SHIELD_LTE_ANSWER_WAIT_S) *
         1000U;
#else
  return 0;
#endif
}

static const struct lpwan_rai_io rai_io = {.set_rai = io_set_rai, .send = io_send, .wait_answer = io_wait_answer};

/* The values of lpwan_rai_opt are those of the socket option. */
BUILD_ASSERT(LPWAN_RAI_OPT_NO_DATA == RAI_NO_DATA && LPWAN_RAI_OPT_LAST == RAI_LAST &&
             LPWAN_RAI_OPT_ONE_RESP == RAI_ONE_RESP);

/* The send ends with its result before the watchdog of service.c fails it (SEND_TIMEOUT_S), with 10 s for the socket.
 */
#ifdef CONFIG_LPWAN_SHIELD_RAI
BUILD_ASSERT(CONFIG_LPWAN_SHIELD_LTE_ANSWER_WAIT_S + 10 <= CONFIG_LPWAN_SHIELD_SEND_TIMEOUT_S &&
                 CONFIG_LPWAN_SHIELD_LTE_ANSWER_WAIT_NBIOT_S + 10 <= CONFIG_LPWAN_SHIELD_SEND_TIMEOUT_S,
             "the answer wait must be shorter than the send timeout");
#endif

/* The modem trace region (debug.conf) is hand-placed in app.overlay: it must follow the IPC regions of the modem
 * library, lie inside the modem RAM and be as large as the library expects. */
#if defined(CONFIG_NRF_MODEM_LIB_TRACE) && !defined(CONFIG_PARTITION_MANAGER_ENABLED)
#define TRACE_NODE DT_NODELABEL(cpucell_cpuapp_ipc_shm_trace)
#define RX_NODE DT_NODELABEL(cpucell_cpuapp_ipc_shm_heap)
#define MODEM_NODE DT_NODELABEL(sram0_ns_modem)
BUILD_ASSERT(DT_REG_ADDR(TRACE_NODE) >= DT_REG_ADDR(RX_NODE) + DT_REG_SIZE(RX_NODE),
             "modem trace region overlaps the IPC regions of the modem library");
BUILD_ASSERT(DT_REG_ADDR(TRACE_NODE) + DT_REG_SIZE(TRACE_NODE) <= DT_REG_ADDR(MODEM_NODE) + DT_REG_SIZE(MODEM_NODE),
             "modem trace region is outside the modem RAM");
BUILD_ASSERT(DT_REG_SIZE(TRACE_NODE) >= CONFIG_NRF_MODEM_LIB_SHMEM_TRACE_SIZE,
             "modem trace region is smaller than NRF_MODEM_LIB_SHMEM_TRACE_SIZE");
#endif

static void worker_send(const struct msg *m) {
  struct lpwan_rai_out out = {0};
  sensei_LteRai flag = m->rai;
  uint32_t wait_ms = 0;
  uint32_t attempts = 0;
  bool unconfirmed;

  if (!registered) {
    svc_emit_tx_done(m->seq, false, 0, 0); /* coverage lost: fail at once, the master retries later */
    return;
  }
  if (!IS_ENABLED(CONFIG_LPWAN_SHIELD_RAI)) {
    flag = sensei_LteRai_LTE_RAI_OFF;
  } else if (!cfg.udp && flag == sensei_LteRai_LTE_RAI_ONE_RESP) {
    flag = sensei_LteRai_LTE_RAI_LAST; /* the answer of a TCP server is a stream, not one datagram */
  }
  if (flag == sensei_LteRai_LTE_RAI_ONE_RESP && lpwan_answer_key(m->data, m->len) != 0) {
    wait_ms = answer_wait_ms();
    k_sem_reset(&answer_sem);
    atomic_set(&wait_key, (atomic_val_t)lpwan_answer_key(m->data, m->len));
  } else if (flag == sensei_LteRai_LTE_RAI_ONE_RESP) {
    flag = sensei_LteRai_LTE_RAI_LAST; /* no chunk header to match an answer with */
  }
  /* A send that failed is tried once more on a new socket. A datagram that went out is not sent again, also if the
   * answer did not come: that is reported as "sent, unconfirmed" and the client decides. */
  for (int i = 0; i < 2 && !out.sent; i++) {
    attempts++;
    if (sock < 0 && open_socket() != 0) {
      continue;
    }
    lpwan_rai_send(&rai_io, NULL, flag, m->data, m->len, wait_ms, &out);
    if (out.rai_errors > 0) {
      LOG_WRN("RAI option refused (%u): the modem or the network does not support it", out.rai_errors);
    }
    if (!out.sent) {
      LOG_WRN("send failed, reopen socket");
      close_socket();
    }
  }
  atomic_set(&wait_key, 0);
  unconfirmed = lpwan_rai_unconfirmed(flag, wait_ms, &out);
  if (wait_ms != 0 && out.sent) {
    (void)atomic_inc(out.answered ? &n_answers : &n_no_answer);
    if (unconfirmed) {
      LOG_WRN("sent, no answer of the server within %u s", (unsigned)(wait_ms / 1000));
    } else {
      LOG_INF("send answered");
    }
  }
  svc_emit_tx_done_ack(m->seq, lpwan_rai_send_ok(&out), attempts, out.answered, unconfirmed);
}

/* The connection evaluation of the registered modem. The result is an event; without one the master sends as usual. */
static void worker_eval(void) {
  sensei_LpwanEvent e = sensei_LpwanEvent_init_zero;
  sensei_ConnEval *c = &e.conn_eval;
#ifdef CONFIG_LTE_LC_CONN_EVAL_MODULE
  struct lte_lc_conn_eval_params p;
  int err = registered && modem_ready ? lte_lc_conn_eval_params_get(&p) : -ENOTCONN;

  if (err == 0) {
    c->valid = true;
    c->energy = lpwan_energy_from_modem((int)p.energy_estimate);
    c->rsrp_valid = p.rsrp != LTE_LC_CELL_RSRP_INVALID;
    c->rsrp_dbm = c->rsrp_valid ? lpwan_rsrp_dbm(p.rsrp) : 0;
    c->rsrq_db = p.rsrq == LTE_LC_CELL_RSRQ_INVALID ? 0 : lpwan_rsrq_db(p.rsrq);
    c->ce_level = p.ce_level <= LTE_LC_CE_LEVEL_3 ? (uint32_t)p.ce_level : 0;
    c->tx_repetitions = p.tx_rep > 0 ? (uint32_t)p.tx_rep : 0;
    if (c->rsrp_valid) {
      last_rsrp_dbm = c->rsrp_dbm;
      last_rsrp_valid = true;
    }
    LOG_INF("conneval: energy %d, RSRP %s%d dBm, CE %u, TX repetitions %u", (int)c->energy,
            c->rsrp_valid ? "" : "invalid ", (int)c->rsrp_dbm, (unsigned)c->ce_level, (unsigned)c->tx_repetitions);
  } else {
    LOG_WRN("conneval: no result (%d)", err);
  }
#endif
  c->rai_support = rai_support;
  e.which_body = sensei_LpwanEvent_conn_eval_tag;
  svc_emit(&e);
}

static void lte_main(void *a, void *b, void *c) {
  static struct msg m;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    k_msgq_get(&lte_q, &m, K_FOREVER);
    switch (m.cmd) {
    case CMD_CONNECT:
      worker_connect();
      break;
    case CMD_SEND:
      worker_send(&m);
      break;
    case CMD_TIME:
      emit_time();
      break;
    case CMD_EVAL:
      worker_eval();
      break;
    case CMD_DISCONNECT:
    case CMD_SLEEP_DEEP:
      want_connected = false;
      close_socket();
      if (modem_ready) {
        (void)(m.cmd == CMD_SLEEP_DEEP ? lte_lc_power_off() : lte_lc_offline());
      }
      if (m.cmd == CMD_DISCONNECT) {
        svc_emit_radio(sensei_LpwanEvent_disconnected_tag, radio_id());
      }
      break;
    }
  }
}

static void rx_main(void *a, void *b, void *c) {
  static uint8_t buf[1024];

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    struct zsock_pollfd pfd = {.fd = sock, .events = ZSOCK_POLLIN};

    if (sock < 0) {
      k_msleep(200);
      continue;
    }
    if (zsock_poll(&pfd, 1, 2000) > 0 && (pfd.revents & ZSOCK_POLLIN)) {
      ssize_t n = zsock_recv(sock, buf, sizeof(buf), 0);

      if (n > 0) {
        struct lpwan_rx rx = lpwan_rx_decide(buf, (size_t)n, (uint32_t)atomic_get(&wait_key));

        if (rx.downlink) {
          /* A downlink is an answer too, and it goes to the node first. */
          svc_emit_downlink(radio_id(), cfg.server_port, buf, (size_t)n);
        } else if (!rx.answer) {
          LOG_DBG("late ack dropped");
        }
        if (rx.answer) {
          k_sem_give(&answer_sem);
        }
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        /* TCP closed by the server, or a socket error: reopen on the next send. */
        close_socket();
      }
    }
  }
}

/* ---- API ---- */

int lte_init(void) {
  /* The modem library stays off until the first connect. */
  k_thread_create(&lte_thread, lte_stack, K_THREAD_STACK_SIZEOF(lte_stack), lte_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_WORKER_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&lte_thread, "lte");
  k_thread_create(&rx_thread, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_WORKER_PRIO + 1), 0, K_NO_WAIT);
  k_thread_name_set(&rx_thread, "lte_rx");
  return 0;
}

sensei_LpwanResult lte_configure(const sensei_LteConfigure *c) {
  if (c->server_port > 65535 || c->mode > sensei_LteMode_LTE_NB_IOT) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  k_mutex_lock(&cfg_lock, K_FOREVER);
  cfg = *c;
  configured = true;
  k_mutex_unlock(&cfg_lock);
  return sensei_LpwanResult_LPWAN_OK;
}

static sensei_LpwanResult post(enum cmd cmd, uint32_t seq, const uint8_t *data, size_t len, sensei_LteRai rai) {
  static struct msg m; /* only the link thread posts */

  m.cmd = cmd;
  m.seq = seq;
  m.len = (uint16_t)len;
  m.rai = rai;
  if (len) {
    memcpy(m.data, data, len);
  }
  return k_msgq_put(&lte_q, &m, K_NO_WAIT) == 0 ? sensei_LpwanResult_LPWAN_OK : sensei_LpwanResult_LPWAN_BUSY;
}

sensei_LpwanResult lte_connect(void) {
  if (!configured || cfg.server_host[0] == '\0' || cfg.server_port == 0) {
    return sensei_LpwanResult_LPWAN_NOT_CONFIGURED;
  }
  want_connected = true;
  connect_cancelled = false; /* a new request: an earlier disconnect does not cancel it (do_connect() checks both) */
  return post(CMD_CONNECT, 0, NULL, 0, sensei_LteRai_LTE_RAI_OFF);
}

sensei_LpwanResult lte_disconnect(void) {
  /* Now, not when the worker gets to it: the worker may sit in the registration wait for minutes. The
   * semaphore ends that wait; do_connect() sees the cancel flag (or want_connected cleared) and returns without a
   * connection. Only lte_connect() clears the flag, so a disconnect is not lost when it comes before do_connect()
   * starts. */
  want_connected = false;
  connect_cancelled = true;
  k_sem_give(&registered_sem);
  return post(CMD_DISCONNECT, 0, NULL, 0, sensei_LteRai_LTE_RAI_OFF);
}

sensei_LpwanResult lte_send(uint32_t seq, uint32_t port, const uint8_t *data, size_t len, sensei_LteRai rai) {
  ARG_UNUSED(port); /* UDP/TCP: the destination port is part of the configuration */
  if (len == 0 || len > sizeof(((struct msg *)0)->data) || (unsigned)rai > sensei_LteRai_LTE_RAI_ONE_RESP) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  return post(CMD_SEND, seq, data, len, rai);
}

sensei_LpwanResult lte_eval(void) {
#ifdef CONFIG_LTE_LC_CONN_EVAL_MODULE
  return post(CMD_EVAL, 0, NULL, 0, sensei_LteRai_LTE_RAI_OFF);
#else
  return sensei_LpwanResult_LPWAN_UNSUPPORTED;
#endif
}

sensei_LpwanResult lte_request_time(void) { return post(CMD_TIME, 0, NULL, 0, sensei_LteRai_LTE_RAI_OFF); }

sensei_LpwanResult lte_sleep(uint32_t deep) {
  /* deep = 0: the modem sleeps by itself in PSM, nothing to do. */
  return deep ? post(CMD_SLEEP_DEEP, 0, NULL, 0, sensei_LteRai_LTE_RAI_OFF) : sensei_LpwanResult_LPWAN_OK;
}

void lte_fill_status(sensei_LpwanStatus *st) {
  st->lte_connected = registered && sock >= 0;
  if (modem_ready && registered) {
    int rsrp = 255;

    if (nrf_modem_at_scanf("AT+CESQ", "+CESQ: %*d,%*d,%*d,%*d,%*d,%d", &rsrp) == 1) {
      last_rsrp_valid = rsrp != 255; /* 255: not known or not detectable */
      if (last_rsrp_valid) {
        last_rsrp_dbm = rsrp - 140;
      }
    }
    st->rssi_valid = last_rsrp_valid;
    st->rssi_dbm = last_rsrp_valid ? last_rsrp_dbm : 0;
  }
  st->rai_support = rai_support;
  st->lte_answers = (uint32_t)atomic_get(&n_answers);
  st->lte_no_answer = (uint32_t)atomic_get(&n_no_answer);
  k_spinlock_key_t k = k_spin_lock(&cnt_lock);

  st->has_rrc_connected_ms = st->has_attaches = st->has_psm_entries = true;
  st->rrc_connected_ms = lpwan_cnt_rrc_ms(&cnt, k_uptime_get_32());
  st->attaches = cnt.attaches;
  st->psm_entries = cnt.psm_entries;
  k_spin_unlock(&cnt_lock, k);
}

void lte_modem_string(char *out, size_t cap) { snprintf(out, cap, "%s", modem_ver); }
