/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <math.h>
#include <stdio.h>

#include "sensei_sensor_math.h"

static int fails;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                              \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void test_sgp41(void) {
  CHECK(sensei_sgp41_rh_ticks(50.0f) == 32768);
  CHECK(sensei_sgp41_rh_ticks(0.0f) == 0);
  CHECK(sensei_sgp41_rh_ticks(100.0f) == 65535);
  CHECK(sensei_sgp41_rh_ticks(150.0f) == 65535);
  CHECK(sensei_sgp41_rh_ticks(-5.0f) == 0);
  CHECK(sensei_sgp41_t_ticks(25.0f) == 0x6666);
  CHECK(sensei_sgp41_t_ticks(-45.0f) == 0);
  CHECK(sensei_sgp41_t_ticks(130.0f) == 65535);
  CHECK(sensei_sgp41_t_ticks(-100.0f) == 0);
}

static void test_as7331(void) {
  CHECK(fabsf(sensei_as7331_temp_c(1338) - 0.0f) < 0.01f);
  CHECK(fabsf(sensei_as7331_temp_c(1858) - 26.0f) < 0.01f);

  /* datasheet Figure 7: 385 / 347 / 794 counts per uW/cm2 at 2048x (gain 0) and 64 ms (time 6) */
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 385, 0, 6) - 1.0f) < 1e-4f);
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVB, 347, 0, 6) - 1.0f) < 1e-4f);
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVC, 794, 0, 6) - 1.0f) < 1e-4f);
  /* half the gain or half the time: twice the irradiance for the same counts */
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 385, 1, 6) - 2.0f) < 1e-4f);
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 385, 0, 5) - 2.0f) < 1e-4f);
  /* gain 2x (10), 2048 ms (11): LSB 83 nW/cm2, full scale 5448 uW/cm2 */
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 1, 10, 11) - 0.0831f) < 1e-4f);
  CHECK(fabsf(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 65535, 10, 11) - 5448.0f) < 5.0f);
  CHECK(sensei_as7331_uw_cm2(SENSEI_AS7331_UVA, 0, 10, 11) == 0.0f);
  CHECK(sensei_as7331_uw_cm2((enum sensei_as7331_ch)7, 5, 10, 11) == 0.0f);
}

static void test_scd41_bh1730(void) {
  CHECK(sensei_scd41_asc_hours(44, 300) == 44);
  CHECK(sensei_scd41_asc_hours(156, 300) == 156);
  CHECK(sensei_scd41_asc_hours(44, 600) == 24);   /* 22 -> multiple of 4 */
  CHECK(sensei_scd41_asc_hours(156, 3600) == 12); /* 13 -> 12 */
  CHECK(sensei_scd41_asc_hours(44, 3600) == 4);   /* 3.7 -> min 4 */
  CHECK(sensei_scd41_asc_hours(44, 0) == 44);

  CHECK(sensei_bh1730_meas_ms(0xED) == 108); /* 19 * 2.7 ms * 2 = 102.6 + 5 */
}

int main(void) {
  test_sgp41();
  test_as7331();
  test_scd41_bh1730();
  if (fails) {
    printf("%d check(s) failed\n", fails);
    return 1;
  }
  printf("sensei_sensor_math: all checks passed\n");
  return 0;
}
