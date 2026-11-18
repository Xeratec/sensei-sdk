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
 *   gnss blocked                                          GNSS is blocked by LTE from the start and stays so
 *   gnss error                                            nrf_modem_gnss_start() fails
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
  int32_t lat_e7, lon_e7, alt_m;
  uint32_t acc_m, sats;
} sc = {SC_FIX, 3, 473764000, 85476000, 410, 5, 8};

static nrf_modem_gnss_event_handler_type_t handler;
static struct k_work_delayable fix_work, retry_work;
static bool inited, running;
static uint16_t retry_s = 60;
static uint16_t interval_s = 1;
static struct nrf_modem_gnss_pvt_data_frame frame;

static void emit(int event) {
  if (handler != NULL) {
    handler(event);
  }
}

static void fix_fn(struct k_work *w) {
  int64_t ms;
  int64_t s;
  struct nrf_modem_gnss_pvt_data_frame f;

  ARG_UNUSED(w);
  if (!running) {
    return;
  }
  ms = lpwan_sim_utc_ms();
  s = ms / 1000;
  memset(&f, 0, sizeof(f));
  f.latitude = sc.lat_e7 / 1e7;
  f.longitude = sc.lon_e7 / 1e7;
  f.altitude = (float)sc.alt_m;
  f.accuracy = (float)sc.acc_m;
  f.flags = NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID;
  {
    int64_t days = s / 86400, rem = s % 86400;
    /* civil date from days since 1970 (Howard Hinnant's algorithm) */
    int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;

    f.datetime.year = (uint16_t)(y + (m <= 2));
    f.datetime.month = (uint8_t)m;
    f.datetime.day = (uint8_t)d;
    f.datetime.hour = (uint8_t)(rem / 3600);
    f.datetime.minute = (uint8_t)(rem % 3600 / 60);
    f.datetime.seconds = (uint8_t)(rem % 60);
    f.datetime.ms = (uint16_t)(ms % 1000);
  }
  for (uint32_t i = 0; i < sc.sats && i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
    f.sv[i].sv = (uint16_t)(i + 1);
    f.sv[i].flags = NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX;
  }
  frame = f;
  LOG_INF("gnss: fix");
  emit(NRF_MODEM_GNSS_EVT_FIX);
  running = false;
}

static void retry_fn(struct k_work *w) {
  ARG_UNUSED(w);
  if (running) {
    running = false;
    LOG_INF("gnss: fix retry time over");
    emit(NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT);
  }
}

int32_t nrf_modem_gnss_event_handler_set(nrf_modem_gnss_event_handler_type_t h) {
  if (!inited) {
    inited = true;
    k_work_init_delayable(&fix_work, fix_fn);
    k_work_init_delayable(&retry_work, retry_fn);
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

int32_t nrf_modem_gnss_start(void) {
  if (sc.mode == SC_ERROR) {
    return -EINVAL;
  }
  running = true;
  LOG_INF("gnss: start (%s, fix interval %u, retry %u s)",
          sc.mode == SC_FIX ? "fix" : (sc.mode == SC_BLOCKED ? "blocked" : "timeout"), interval_s, retry_s);
  if (sc.mode == SC_BLOCKED) {
    emit(NRF_MODEM_GNSS_EVT_BLOCKED);
  } else if (sc.mode == SC_FIX) {
    (void)k_work_reschedule(&fix_work, K_SECONDS(sc.delay_s));
  }
  if (retry_s > 0) {
    (void)k_work_reschedule(&retry_work, K_SECONDS(retry_s));
  }
  return 0;
}

int32_t nrf_modem_gnss_stop(void) {
  running = false;
  (void)k_work_cancel_delayable(&fix_work);
  (void)k_work_cancel_delayable(&retry_work);
  return 0;
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
  } else if (argc >= 1 && strcmp(argv[0], "error") == 0) {
    sc.mode = SC_ERROR;
  } else {
    LOG_WRN("unknown gnss command");
    return;
  }
  LOG_INF("gnss scenario: %s", argv[0]);
}
