/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * The LTE modem of the shield for native_sim: a small fake of the modem library, lte_lc and date_time with the
 * signatures of the real ones (the headers are those of NCS), so that lte.c runs unchanged. The network is the host:
 * the UDP socket of lte.c is an offloaded host socket (CONFIG_NET_NATIVE_OFFLOADED_SOCKETS) and reaches the collector.
 *
 * What it does: registration after a delay (home network), PSM, eDRX and RAI reports, connection evaluation, network
 * time from the host clock, AT commands for the modem version and the signal, RRC changes on command (`modem rrc`, for
 * the status counters). What it does not: automatic RRC states, modem sleep events, paging, radio timing, the RAI
 * option on the air (the option is only counted and reported). Commands of the harness
 * ("modem ..."): see README.md.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <date_time.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "../src/lte.h"
#include "sensei_lpwan/lpwan_sim.h"
#include "sim_shield.h"

LOG_MODULE_REGISTER(sim_modem, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define REG_DELAY_MS 2000 /* from connect to the registration */

static lte_lc_evt_handler_t handler;
static struct k_work_delayable reg_work;
static K_MUTEX_DEFINE(lock);
static bool lib_up;
static bool lte_on;  /* functional mode: LTE active */
static bool gnss_on; /* functional mode: GNSS active */
static int func_off; /* 0 or 4: the mode of the modem while both are off */
static enum lte_lc_system_mode sys_mode = LTE_LC_SYSTEM_MODE_LTEM;
static bool want_reg;      /* connect asked, no offline since */
static bool registered;    /* what the modem told the app */
static bool cover = true;  /* the network is there */
static bool rai_ok = true; /* the cell supports RAI */
static bool psm_req;       /* PSM requested by the app */
static int psm_tau, psm_active = -1;
static int64_t time_at = -1; /* uptime from which the network time is known, -1: not yet */
static int64_t boot_utc_ms;
static int energy = LTE_LC_ENERGY_CONSUMPTION_NORMAL;
static unsigned n_rai[8];

static void emit(const struct lte_lc_evt *evt) {
  if (handler != NULL) {
    handler(evt);
  }
}

static void emit_status(enum lte_lc_nw_reg_status s) {
  struct lte_lc_evt e = {.type = LTE_LC_EVT_NW_REG_STATUS, .nw_reg_status = s};

  emit(&e);
}

static void reg_fn(struct k_work *w) {
  ARG_UNUSED(w);
  k_mutex_lock(&lock, K_FOREVER);
  if (!want_reg) {
    k_mutex_unlock(&lock);
    return;
  }
  if (!cover) {
    k_mutex_unlock(&lock);
    emit_status(LTE_LC_NW_REG_SEARCHING);
    (void)k_work_reschedule(&reg_work, K_MSEC(5000));
    return;
  }
  registered = true;
  time_at = k_uptime_get(); /* the network time comes with the registration */
  k_mutex_unlock(&lock);
  LOG_INF("modem: registered (home network)");
  emit_status(LTE_LC_NW_REG_REGISTERED_HOME);
  {
    struct lte_lc_evt psm = {.type = LTE_LC_EVT_PSM_UPDATE,
                             .psm_cfg = {.tau = psm_req ? psm_tau : -1, .active_time = psm_req ? psm_active : -1}};
    struct lte_lc_evt rai = {.type = LTE_LC_EVT_RAI_UPDATE,
                             .rai_cfg = {.cell_id = 1, .mcc = 001, .mnc = 01, .as_rai = rai_ok, .cp_rai = rai_ok}};

    emit(&psm);
    emit(&rai);
  }
}

/* ---- modem library ---- */

int nrf_modem_lib_init(void) {
  if (!lib_up) {
    lib_up = true;
    k_work_init_delayable(&reg_work, reg_fn);
    boot_utc_ms = lpwan_sim_utc_ms() - k_uptime_get();
    LOG_INF("modem library started (simulated)");
  }
  return 0;
}

int nrf_modem_lib_shutdown(void) {
  lib_up = false;
  return 0;
}

/* ---- lte_lc ---- */

void lte_lc_register_handler(lte_lc_evt_handler_t h) { handler = h; }

static bool mode_has_gnss(enum lte_lc_system_mode m) {
  return m == LTE_LC_SYSTEM_MODE_GPS || m == LTE_LC_SYSTEM_MODE_LTEM_GPS || m == LTE_LC_SYSTEM_MODE_NBIOT_GPS ||
         m == LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
}

/* The system mode can be set only with the modem off or offline (as on the real one). */
int lte_lc_system_mode_set(enum lte_lc_system_mode mode, enum lte_lc_system_mode_preference pref) {
  ARG_UNUSED(pref);
  if (lte_on || gnss_on) {
    LOG_WRN("modem: system mode refused, the modem is on");
    return -EFAULT;
  }
  sys_mode = mode;
  LOG_INF("modem: system mode %s%s",
          mode == LTE_LC_SYSTEM_MODE_NBIOT || mode == LTE_LC_SYSTEM_MODE_NBIOT_GPS ? "NB-IoT" : "LTE-M",
          mode_has_gnss(mode) ? " with GNSS" : "");
  return 0;
}

int lte_lc_system_mode_get(enum lte_lc_system_mode *mode, enum lte_lc_system_mode_preference *pref) {
  *mode = sys_mode;
  *pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
  return 0;
}

bool sim_modem_gnss_active(void) { return gnss_on; }

static int go_off(void);

int lte_lc_func_mode_get(enum lte_lc_func_mode *mode) {
  if (lte_on && gnss_on) {
    *mode = LTE_LC_FUNC_MODE_NORMAL;
  } else if (lte_on) {
    *mode = mode_has_gnss(sys_mode) ? LTE_LC_FUNC_MODE_ACTIVATE_LTE : LTE_LC_FUNC_MODE_NORMAL;
  } else if (gnss_on) {
    *mode = LTE_LC_FUNC_MODE_ACTIVATE_GNSS;
  } else {
    *mode = func_off == 4 ? LTE_LC_FUNC_MODE_OFFLINE : LTE_LC_FUNC_MODE_POWER_OFF;
  }
  return 0;
}

/* The functional modes that the shield uses: 0, 4 (both off), 1 (LTE, and GNSS if it is in the system mode), 20, 21
 * (LTE off, on), 30, 31 (GNSS off, on). */
int lte_lc_func_mode_set(enum lte_lc_func_mode mode) {
  bool gnss_was = gnss_on;

  switch (mode) {
  case LTE_LC_FUNC_MODE_POWER_OFF:
  case LTE_LC_FUNC_MODE_OFFLINE:
    func_off = mode == LTE_LC_FUNC_MODE_OFFLINE ? 4 : 0;
    gnss_on = false;
    lte_on = false;
    (void)go_off();
    break;
  case LTE_LC_FUNC_MODE_NORMAL:
    lte_on = true;
    gnss_on = mode_has_gnss(sys_mode);
    break;
  case LTE_LC_FUNC_MODE_ACTIVATE_LTE:
    lte_on = true;
    break;
  case LTE_LC_FUNC_MODE_DEACTIVATE_LTE:
    lte_on = false;
    (void)go_off();
    break;
  case LTE_LC_FUNC_MODE_ACTIVATE_GNSS:
    if (!mode_has_gnss(sys_mode)) {
      LOG_WRN("modem: functional mode 31 refused, no GNSS in the system mode");
      return -EFAULT;
    }
    gnss_on = true;
    break;
  case LTE_LC_FUNC_MODE_DEACTIVATE_GNSS:
    gnss_on = false;
    break;
  default:
    return -EINVAL;
  }
  LOG_INF("modem: functional mode %d", (int)mode);
#ifdef CONFIG_LPWAN_SHIELD_GNSS
  if (gnss_was && !gnss_on) {
    sim_gnss_deactivated();
  }
#else
  ARG_UNUSED(gnss_was);
#endif
  return 0;
}

int lte_lc_psm_param_set(const char *rptau, const char *rat) {
  ARG_UNUSED(rptau);
  ARG_UNUSED(rat);
  return 0;
}

int lte_lc_psm_req(bool enable) {
  psm_req = enable;
  psm_tau = 3600; /* what the network grants */
  psm_active = 10;
  return 0;
}

int lte_lc_edrx_param_set(enum lte_lc_lte_mode mode, const char *edrx) {
  ARG_UNUSED(mode);
  ARG_UNUSED(edrx);
  return 0;
}

int lte_lc_ptw_set(enum lte_lc_lte_mode mode, const char *ptw) {
  ARG_UNUSED(mode);
  ARG_UNUSED(ptw);
  return 0;
}

int lte_lc_edrx_req(bool enable) {
  ARG_UNUSED(enable);
  return 0;
}

int lte_lc_connect_async(lte_lc_evt_handler_t h) {
  if (h != NULL) {
    handler = h;
  }
  k_mutex_lock(&lock, K_FOREVER);
  want_reg = true;
  k_mutex_unlock(&lock);
  (void)lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL); /* the library sets functional mode 1 */
  (void)k_work_reschedule(&reg_work, K_MSEC(REG_DELAY_MS));
  return 0;
}

static int go_off(void) {
  bool was;

  k_mutex_lock(&lock, K_FOREVER);
  want_reg = false;
  was = registered;
  registered = false;
  time_at = -1;
  k_mutex_unlock(&lock);
  (void)k_work_cancel_delayable(&reg_work);
  if (was) {
    emit_status(LTE_LC_NW_REG_NOT_REGISTERED);
  }
  return 0;
}

int lte_lc_offline(void) { return lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE); }

int lte_lc_power_off(void) { return lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF); }

int lte_lc_conn_eval_params_get(struct lte_lc_conn_eval_params *p) {
  if (!registered) {
    return -EOPNOTSUPP;
  }
  memset(p, 0, sizeof(*p));
  p->rrc_state = LTE_LC_RRC_MODE_IDLE;
  p->energy_estimate = (enum lte_lc_energy_estimate)energy;
  p->ce_level = energy >= LTE_LC_ENERGY_CONSUMPTION_NORMAL ? LTE_LC_CE_LEVEL_0 : LTE_LC_CE_LEVEL_2;
  p->rsrp = 45; /* -95 dBm */
  p->rsrq = 30;
  p->tx_rep = energy >= LTE_LC_ENERGY_CONSUMPTION_NORMAL ? 1 : 32;
  return 0;
}

/* ---- date_time ---- */

int date_time_now(int64_t *unix_time_ms) {
  if (time_at < 0 || k_uptime_get() < time_at) {
    return -ENODATA;
  }
  *unix_time_ms = boot_utc_ms + k_uptime_get();
  return 0;
}

int date_time_update_async(date_time_evt_handler_t h) {
  struct date_time_evt e = {.type = DATE_TIME_OBTAINED_MODEM};

  if (h != NULL && time_at >= 0) {
    h(&e);
  }
  return 0;
}

/* ---- AT commands ---- */

int nrf_modem_at_printf(const char *fmt, ...) {
  char cmd[96];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);
  LOG_DBG("modem: AT %s", cmd);
  return 0;
}

int nrf_modem_at_scanf(const char *cmd, const char *fmt, ...) {
  const char *answer = NULL;
  char buf[64];
  va_list ap;
  int n;

  if (strcmp(cmd, "AT%SHORTSWVER") == 0) {
    answer = "%SHORTSWVER: mfw_nrf91x1_2.0.2-sim";
  } else if (strcmp(cmd, "AT+CESQ") == 0) {
    snprintf(buf, sizeof(buf), "+CESQ: 99,99,255,255,%d,%d", 30, registered ? 45 : 255);
    answer = buf;
  } else {
    return -1;
  }
  va_start(ap, fmt);
  n = vsscanf(answer, fmt, ap);
  va_end(ap);
  return n;
}

/* ---- the RAI socket option (lte.c: the host socket does not know it) ---- */

int sim_modem_set_rai(int opt) {
  static const char *const names[] = {"?", "no data", "last", "one response", "ongoing", "wait more"};

  if (opt < 1 || opt >= (int)ARRAY_SIZE(names)) {
    return -EINVAL;
  }
  n_rai[opt]++;
  LOG_INF("modem: RAI option %d (%s)", opt, names[opt]);
  (void)sim_ctl_send("lte_rai %d", opt);
  return rai_ok ? 0 : -EOPNOTSUPP;
}

/* ---- commands of the harness ---- */

extern void nrf_modem_fault_handler(struct nrf_modem_fault_info *fault);

void sim_modem_command(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[0], "cover") == 0) {
    bool on = argv[1][0] != '0';

    k_mutex_lock(&lock, K_FOREVER);
    cover = on;
    k_mutex_unlock(&lock);
    LOG_INF("modem: coverage %s", on ? "back" : "lost");
    if (!on && registered) {
      registered = false;
      time_at = -1;
      emit_status(LTE_LC_NW_REG_SEARCHING);
    }
    if (want_reg) {
      (void)k_work_reschedule(&reg_work, K_MSEC(REG_DELAY_MS));
    }
  } else if (argc >= 2 && strcmp(argv[0], "rai") == 0) {
    rai_ok = argv[1][0] != '0';
  } else if (argc >= 2 && strcmp(argv[0], "rrc") == 0) {
    /* RRC state change for the counters of the status: "modem rrc 1" connected, "modem rrc 0" idle */
    struct lte_lc_evt e = {.type = LTE_LC_EVT_RRC_UPDATE,
                           .rrc_mode = argv[1][0] != '0' ? LTE_LC_RRC_MODE_CONNECTED : LTE_LC_RRC_MODE_IDLE};

    emit(&e);
#ifdef CONFIG_LPWAN_SHIELD_GNSS
    sim_gnss_rrc(argv[1][0] != '0');
#endif
  } else if (argc >= 2 && strcmp(argv[0], "energy") == 0) {
    energy = atoi(argv[1]);
  } else if (argc >= 1 && strcmp(argv[0], "sleep_deep") == 0) {
    /* The LTE worker gets a deep sleep request (power off), as lte_sleep(1) of a client that asks for it (the nRF5340
     * client does not today). With a fix running only LTE goes, and the end of the fix powers the modem off. */
    LOG_INF("modem: deep sleep requested");
    (void)lte_sleep(1);
  } else if (argc >= 1 && strcmp(argv[0], "fault") == 0) {
    struct nrf_modem_fault_info f = {.reason = 0x4D, .program_counter = 0x20001234};

    nrf_modem_fault_handler(&f);
  } else {
    LOG_WRN("unknown modem command");
  }
}
