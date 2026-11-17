/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_sensor_math.h"

uint16_t sensei_sgp41_rh_ticks(float rh_pct) {
  if (rh_pct < 0.0f) {
    rh_pct = 0.0f;
  } else if (rh_pct > 100.0f) {
    rh_pct = 100.0f;
  }
  return (uint16_t)(rh_pct * 65535.0f / 100.0f + 0.5f);
}

uint16_t sensei_sgp41_t_ticks(float t_c) {
  if (t_c < -45.0f) {
    t_c = -45.0f;
  } else if (t_c > 130.0f) {
    t_c = 130.0f;
  }
  return (uint16_t)((t_c + 45.0f) * 65535.0f / 175.0f + 0.5f);
}

float sensei_as7331_temp_c(uint16_t raw) { return raw * 0.05f - 66.9f; }

float sensei_as7331_uw_cm2(enum sensei_as7331_ch ch, uint16_t counts, uint8_t gain, uint8_t time) {
  static const float re_2048x_64ms[3] = {385.0f, 347.0f, 794.0f}; /* counts per uW/cm2 */

  if ((unsigned)ch > SENSEI_AS7331_UVC) {
    return 0.0f;
  }
  if (gain > 11) {
    gain = 11;
  }
  if (time > 14) {
    time = 0; /* TIME 15 is a 1 ms conversion like 0 */
  }
  float gain_x = 2048.0f / (float)(1u << gain);
  float t_ms = (float)(1u << time);
  return (float)counts / (re_2048x_64ms[ch] * (gain_x / 2048.0f) * (t_ms / 64.0f));
}

uint16_t sensei_scd41_asc_hours(uint16_t hours_at_5min, uint32_t period_s) {
  if (period_s == 0) {
    period_s = 300;
  }
  uint32_t h = ((uint32_t)hours_at_5min * 300u + period_s / 2) / period_s;
  h = (h + 2) / 4 * 4;
  if (h < 4) {
    h = 4;
  }
  return (uint16_t)h;
}

uint32_t sensei_bh1730_meas_ms(uint8_t itime) {
  /* Tint = 2.7 ms * (256 - ITIME) per channel, both channels measured: x2, plus margin. */
  return ((256u - itime) * 27u * 2u + 9u) / 10u + 5u;
}
