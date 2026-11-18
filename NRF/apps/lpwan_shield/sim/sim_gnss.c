/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * The GNSS of the modem for native_sim: a fake of the nrf_modem_gnss calls that gnss.c makes (the header is the one of
 * NCS), so that gnss.c runs unchanged. It does not model satellites: a scenario says what the receiver does after the
 * start. Set it with the harness command "gnss ..." or the option -gnss="..." (the same words), see README.md:
 *
 *   gnss fix [delay_s [lat_e7 lon_e7 alt_m acc_m sats]]   a valid fix after delay_s (default 3 s)
 *   gnss timeout                                          no fix: the fix retry time ends the attempt
 *   gnss blocked [unblock_s]                              GNSS is blocked by LTE from the start; with unblock_s it is
 *                                                         unblocked then and the receiver goes on without a fix
 *   gnss error                                            nrf_modem_gnss_start() fails
 *
 * Like the modem: a PVT frame every second while the receiver runs (without a valid fix, or with it at the end), no
 * frames while blocked, BLOCKED and UNBLOCKED events, and nrf_modem_gnss_start() is refused when GNSS is not active
 * (functional mode 31 or 1 with GNSS in the system mode: sim_modem.c keeps the mode). When the modem takes GNSS away
 * during a run (functional mode 30, 20 with LTE only is not enough, 4, 0) the run ends without an event. The RRC state
 * of the harness command "modem rrc" blocks (connected) and unblocks (idle) a running fix: the modem gives LTE
 * priority.
 *
 * The time of the fix is the clock of the host (the same that the fake modem gives as network time).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sensei_lpwan/lpwan_sim.h"
#include "sim_shield.h"

LOG_MODULE_REGISTER(sim_gnss, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

enum scenario { SC_FIX, SC_TIMEOUT, SC_BLOCKED, SC_ERROR };

static struct {
  enum scenario mode;
  uint32_t delay_s;
  uint32_t unblock_s; /* SC_BLOCKED: 0 never */
  int32_t lat_e7, lon_e7, alt_m;
  uint32_t acc_m, sats;
} sc = {SC_FIX, 3, 0, 473764000, 85476000, 410, 5, 8};

static nrf_modem_gnss_event_handler_type_t handler;
static struct k_work_delayable pvt_work, retry_work, unblock_work;
static bool inited, running;
static bool blocked;  /* the modem keeps GNSS from running: no PVT frames */
static bool lte_busy; /* RRC connected: LTE has priority */
static uint16_t retry_s = 60;
static uint16_t interval_s = 1;
static int64_t started_ms, until_ms; /* started_ms: uptime of the start, until_ms: the fix comes from here (SC_FIX) */
static struct nrf_modem_gnss_pvt_data_frame frame;

static void emit(int event) {
  if (handler != NULL) {
    handler(event);
  }
}

static void fill_time(struct nrf_modem_gnss_pvt_data_frame *f) {
  int64_t ms = lpwan_sim_utc_ms();
  int64_t s = ms / 1000;
  int64_t days = s / 86400, rem = s % 86400;
  /* civil date from days since 1970 (Howard Hinnant's algorithm) */
  int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;

  f->datetime.year = (uint16_t)(y + (m <= 2));
  f->datetime.month = (uint8_t)m;
  f->datetime.day = (uint8_t)d;
  f->datetime.hour = (uint8_t)(rem / 3600);
  f->datetime.minute = (uint8_t)(rem % 3600 / 60);
  f->datetime.seconds = (uint8_t)(rem % 60);
  f->datetime.ms = (uint16_t)(ms % 1000);
}

/* One PVT frame per second while the receiver runs and is not blocked. */
static void pvt_fn(struct k_work *w) {
  struct nrf_modem_gnss_pvt_data_frame f;
  bool valid;

  ARG_UNUSED(w);
  if (!running) {
    return;
  }
  (void)k_work_reschedule(&pvt_work, K_SECONDS(1));
  if (blocked) {
    return;
  }
  valid = sc.mode == SC_FIX && k_uptime_get() >= until_ms;
  memset(&f, 0, sizeof(f));
  if (valid) {
    f.latitude = sc.lat_e7 / 1e7;
    f.longitude = sc.lon_e7 / 1e7;
    f.altitude = (float)sc.alt_m;
    f.accuracy = (float)sc.acc_m;
    f.flags = NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;
    for (uint32_t i = 0; i < sc.sats && i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
      f.sv[i].sv = (uint16_t)(i + 1);
      f.sv[i].flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX;
    }
  } else {
    f.sv[0].sv = 1; /* a satellite tracked, none used */
  }
  fill_time(&f);
  frame = f;
  if (valid) {
    LOG_INF("gnss: fix");
    running = false;
    (void)k_work_cancel_delayable(&pvt_work);
    (void)k_work_cancel_delayable(&retry_work);
    emit(NRF_MODEM_GNSS_EVT_FIX);
  } else {
    emit(NRF_MODEM_GNSS_EVT_PVT);
  }
}

static void set_blocked(bool b) {
  if (!running || blocked == b) {
    return;
  }
  blocked = b;
  LOG_INF("gnss: %s", b ? "blocked" : "unblocked");
  emit(b ? NRF_MODEM_GNSS_EVT_BLOCKED : NRF_MODEM_GNSS_EVT_UNBLOCKED);
}

static void unblock_fn(struct k_work *w) {
  ARG_UNUSED(w);
  lte_busy = false;
  set_blocked(false);
}

static void retry_fn(struct k_work *w) {
  ARG_UNUSED(w);
  if (running) {
    running = false;
    (void)k_work_cancel_delayable(&pvt_work);
    LOG_INF("gnss: fix retry time over");
    emit(NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT);
  }
}

int32_t nrf_modem_gnss_event_handler_set(nrf_modem_gnss_event_handler_type_t h) {
  if (!inited) {
    inited = true;
    k_work_init_delayable(&pvt_work, pvt_fn);
    k_work_init_delayable(&retry_work, retry_fn);
    k_work_init_delayable(&unblock_work, unblock_fn);
  }
  handler = h;
  return 0;
}

int32_t nrf_modem_gnss_fix_interval_set(uint16_t v) {
  interval_s = v;
  return 0;
}

int32_t nrf_modem_gnss_fix_retry_set(uint16_t v) {
  retry_s = v;
  return 0;
}

int32_t nrf_modem_gnss_nmea_mask_set(uint16_t mask) {
  ARG_UNUSED(mask);
  return 0;
}

int32_t nrf_modem_gnss_start(void) {
  if (!sim_modem_gnss_active()) {
    LOG_WRN("gnss: start refused, GNSS is not active (functional mode)");
    return -EACCES;
  }
  if (sc.mode == SC_ERROR) {
    return -EINVAL;
  }
  running = true;
  blocked = false;
  started_ms = k_uptime_get();
  until_ms = started_ms + (int64_t)sc.delay_s * 1000;
  LOG_INF("gnss: start (%s, fix interval %u, retry %u s)",
          sc.mode == SC_FIX ? "fix" : (sc.mode == SC_BLOCKED ? "blocked" : "timeout"), interval_s, retry_s);
  (void)k_work_reschedule(&pvt_work, K_SECONDS(1));
  if (sc.mode == SC_BLOCKED) {
    set_blocked(true);
    if (sc.unblock_s > 0) {
      (void)k_work_reschedule(&unblock_work, K_SECONDS(sc.unblock_s));
    }
  } else if (lte_busy) {
    set_blocked(true); /* LTE is in RRC connected: the modem lets LTE go first */
  }
  if (retry_s > 0) {
    (void)k_work_reschedule(&retry_work, K_SECONDS(retry_s));
  }
  return 0;
}

int32_t nrf_modem_gnss_stop(void) {
  running = false;
  blocked = false;
  (void)k_work_cancel_delayable(&pvt_work);
  (void)k_work_cancel_delayable(&retry_work);
  (void)k_work_cancel_delayable(&unblock_work);
  return 0;
}

/* sim_modem.c: GNSS was taken away by a functional mode change. The real modem ends the run without an event. */
void sim_gnss_deactivated(void) {
  if (running) {
    LOG_WRN("gnss: deactivated by the modem while running, no event");
  }
  (void)nrf_modem_gnss_stop();
}

/* sim_modem.c, "modem rrc": the modem gives LTE priority over GNSS. */
void sim_gnss_rrc(bool connected) {
  lte_busy = connected;
  if (sc.mode == SC_BLOCKED) {
    return; /* the scenario decides */
  }
  (void)k_work_cancel_delayable(&unblock_work);
  set_blocked(connected);
}

int32_t nrf_modem_gnss_read(void *buf, int32_t buf_len, int type) {
  if (type != NRF_MODEM_GNSS_DATA_PVT || buf_len < (int32_t)sizeof(frame)) {
    return -EINVAL;
  }
  memcpy(buf, &frame, sizeof(frame));
  return 0;
}

/* ---- scenario ---- */

void sim_gnss_command(int argc, char **argv) {
  if (argc >= 1 && strcmp(argv[0], "fix") == 0) {
    sc.mode = SC_FIX;
    if (argc >= 2) {
      sc.delay_s = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    if (argc >= 7) {
      sc.lat_e7 = (int32_t)strtol(argv[2], NULL, 10);
      sc.lon_e7 = (int32_t)strtol(argv[3], NULL, 10);
      sc.alt_m = (int32_t)strtol(argv[4], NULL, 10);
      sc.acc_m = (uint32_t)strtoul(argv[5], NULL, 10);
      sc.sats = (uint32_t)strtoul(argv[6], NULL, 10);
    }
  } else if (argc >= 1 && strcmp(argv[0], "timeout") == 0) {
    sc.mode = SC_TIMEOUT;
  } else if (argc >= 1 && strcmp(argv[0], "blocked") == 0) {
    sc.mode = SC_BLOCKED;
    sc.unblock_s = argc >= 2 ? (uint32_t)strtoul(argv[1], NULL, 10) : 0;
  } else if (argc >= 1 && strcmp(argv[0], "error") == 0) {
    sc.mode = SC_ERROR;
  } else {
    LOG_WRN("unknown gnss command");
    return;
  }
  LOG_INF("gnss scenario: %s", argv[0]);
}
