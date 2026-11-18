/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_lpwan/lpwan_gnss.h"

bool lpwan_gnss_timeout(uint32_t request_s, uint32_t *out_s) {
  if (request_s > LPWAN_GNSS_TIMEOUT_MAX_S) {
    return false;
  }
  *out_s = request_s == 0 ? LPWAN_GNSS_TIMEOUT_DEFAULT_S : request_s;
  return true;
}

uint32_t lpwan_gnss_wait_ms(uint32_t timeout_s) { return (timeout_s + LPWAN_GNSS_MARGIN_S) * 1000u; }

sensei_LpwanResult lpwan_gnss_admit(const lpwan_sm_t *sm, bool fix_running) {
  if (fix_running) {
    return sensei_LpwanResult_LPWAN_BUSY;
  }
  if (sm->tech == LPWAN_TECH_LTE &&
      (sm->state == LPWAN_ST_CONNECTING || sm->state == LPWAN_ST_SENDING || sm->state == LPWAN_ST_DISCONNECTING)) {
    return sensei_LpwanResult_LPWAN_BUSY;
  }
  return sensei_LpwanResult_LPWAN_OK;
}

bool lpwan_gnss_refuses(const sensei_LpwanRequest *req, const lpwan_sm_t *sm) {
  switch (req->which_body) {
  case sensei_LpwanRequest_lte_configure_tag:
  case sensei_LpwanRequest_lte_connect_tag:
  case sensei_LpwanRequest_lte_eval_tag:
  case sensei_LpwanRequest_sleep_tag:
    return true;
  case sensei_LpwanRequest_lte_disconnect_tag:
    return sm->tech == LPWAN_TECH_LTE; /* a LoRa leave does not touch the modem */
  case sensei_LpwanRequest_send_tag:
    return lpwan_tech_of_radio(req->send.radio) == LPWAN_TECH_LTE;
  default:
    return false;
  }
}

sensei_GnssFixStatus lpwan_gnss_end_status(bool fix, bool cancelled, bool error, bool blocked_at_end) {
  if (fix) {
    return sensei_GnssFixStatus_GNSS_FIX_OK;
  }
  if (cancelled) {
    return sensei_GnssFixStatus_GNSS_FIX_CANCELLED;
  }
  if (error) {
    return sensei_GnssFixStatus_GNSS_FIX_FAILED;
  }
  return blocked_at_end ? sensei_GnssFixStatus_GNSS_FIX_BLOCKED : sensei_GnssFixStatus_GNSS_FIX_TIMEOUT;
}

static bool leap(uint16_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

uint32_t lpwan_gnss_utc_to_unix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                uint8_t second) {
  static const uint16_t before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  uint32_t days;

  if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) {
    return 0;
  }
  if (day > 31 || (month == 2 ? day > (leap(year) ? 29 : 28)
                              : (month == 4 || month == 6 || month == 9 || month == 11) && day > 30)) {
    return 0;
  }
  days =
      (uint32_t)(year - 1970) * 365u + (uint32_t)((year - 1969) / 4); /* leap days of 1972 ... year - 1 (2000 is one) */
  days += before[month - 1] + (uint32_t)(day - 1);
  if (month > 2 && leap(year)) {
    days++;
  }
  return days * 86400u + (uint32_t)hour * 3600u + (uint32_t)minute * 60u + second;
}

uint32_t lpwan_gnss_acc_m(float acc) {
  if (!(acc > 0.0f)) { /* NaN and negative too */
    return 0;
  }
  if (acc >= 65535.0f) {
    return 65535u;
  }
  uint32_t m = (uint32_t)acc;

  return (float)m < acc ? m + 1u : m;
}

int32_t lpwan_gnss_deg_e7(double deg, int limit_deg) {
  double v = deg * 1e7;
  double lim = (double)limit_deg * 1e7;

  if (!(v == v)) { /* NaN */
    return 0;
  }
  v = v > lim ? lim : (v < -lim ? -lim : v);
  return (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
}

const char *lpwan_gnss_status_name(sensei_GnssFixStatus s) {
  switch (s) {
  case sensei_GnssFixStatus_GNSS_FIX_OK:
    return "ok";
  case sensei_GnssFixStatus_GNSS_FIX_TIMEOUT:
    return "timeout";
  case sensei_GnssFixStatus_GNSS_FIX_BLOCKED:
    return "blocked";
  case sensei_GnssFixStatus_GNSS_FIX_BUSY:
    return "busy";
  case sensei_GnssFixStatus_GNSS_FIX_NOT_SUPPORTED:
    return "not supported";
  case sensei_GnssFixStatus_GNSS_FIX_FAILED:
    return "failed";
  case sensei_GnssFixStatus_GNSS_FIX_CANCELLED:
    return "cancelled";
  default:
    return "?";
  }
}
