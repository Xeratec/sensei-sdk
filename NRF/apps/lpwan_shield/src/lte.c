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
#include "sensei_lpwan/lpwan_timers.h"
#include "service.h"

LOG_MODULE_REGISTER(lte, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

enum cmd { CMD_CONNECT, CMD_DISCONNECT, CMD_SEND, CMD_TIME, CMD_SLEEP_DEEP };

struct msg {
  enum cmd cmd;
  uint32_t seq;
  uint16_t len;
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
    break;
  case LTE_LC_EVT_EDRX_UPDATE:
    LOG_INF("eDRX: cycle %d, ptw %d", (int)evt->edrx_cfg.edrx, (int)evt->edrx_cfg.ptw);
    break;
  case LTE_LC_EVT_RRC_UPDATE:
    LOG_DBG("RRC %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
    break;
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

static void worker_send(const struct msg *m) {
  ssize_t n = -1;
  uint32_t attempts = 0;

  if (!registered) {
    svc_emit_tx_done(m->seq, false, 0, 0); /* coverage lost: fail at once, the master retries later */
    return;
  }
  for (int i = 0; i < 2 && n != m->len; i++) {
    attempts++;
    if (sock < 0 && open_socket() != 0) {
      continue;
    }
    n = zsock_send(sock, m->data, m->len, 0);
    if (n != m->len) {
      LOG_WRN("send failed (%d), reopen socket", (int)n);
      close_socket();
    }
  }
  svc_emit_tx_done(m->seq, n == m->len, attempts, 0);
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
        svc_emit_downlink(radio_id(), cfg.server_port, buf, (size_t)n);
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

static sensei_LpwanResult post(enum cmd cmd, uint32_t seq, const uint8_t *data, size_t len) {
  static struct msg m; /* only the link thread posts */

  m.cmd = cmd;
  m.seq = seq;
  m.len = (uint16_t)len;
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
  return post(CMD_CONNECT, 0, NULL, 0);
}

sensei_LpwanResult lte_disconnect(void) {
  /* Now, not when the worker gets to it: the worker may sit in the registration wait for minutes. The
   * semaphore ends that wait; do_connect() sees the cancel flag (or want_connected cleared) and returns without a
   * connection. Only lte_connect() clears the flag, so a disconnect is not lost when it comes before do_connect()
   * starts. */
  want_connected = false;
  connect_cancelled = true;
  k_sem_give(&registered_sem);
  return post(CMD_DISCONNECT, 0, NULL, 0);
}

sensei_LpwanResult lte_send(uint32_t seq, uint32_t port, const uint8_t *data, size_t len) {
  ARG_UNUSED(port); /* UDP/TCP: the destination port is part of the configuration */
  if (len == 0 || len > sizeof(((struct msg *)0)->data)) {
    return sensei_LpwanResult_LPWAN_INVALID;
  }
  return post(CMD_SEND, seq, data, len);
}

sensei_LpwanResult lte_request_time(void) { return post(CMD_TIME, 0, NULL, 0); }

sensei_LpwanResult lte_sleep(uint32_t deep) {
  /* deep = 0: the modem sleeps by itself in PSM, nothing to do. */
  return deep ? post(CMD_SLEEP_DEEP, 0, NULL, 0) : sensei_LpwanResult_LPWAN_OK;
}

void lte_fill_status(sensei_LpwanStatus *st) {
  st->lte_connected = registered && sock >= 0;
  if (modem_ready && registered) {
    int rsrp = 255;

    if (nrf_modem_at_scanf("AT+CESQ", "+CESQ: %*d,%*d,%*d,%*d,%*d,%d", &rsrp) == 1 && rsrp != 255) {
      last_rsrp_dbm = rsrp - 140;
    }
    st->rssi_dbm = last_rsrp_dbm;
  }
}

void lte_modem_string(char *out, size_t cap) { snprintf(out, cap, "%s", modem_ver); }
