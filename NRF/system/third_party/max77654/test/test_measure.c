// Copyright (c) 2022 Philip Schilk.
// SPDX-License-Identifier: Apache-2.0

#include "max77654.h"
#include "mock_device.h"

#include "stdlib.h"
#include "string.h"
#include "unity.h"
#include "unity_internals.h"

#include "max77654.c"

struct max77654_h h;

#define IS_WITHIN_1PERC(_want_, _is_) (((_is_) * 100) >= ((_want_) * 99) && ((_is_) * 100) <= ((_want_) * 101))
#define TEST_ASSERT_WITHIN_1PERC(_want_, _is_)                                                                         \
  TEST_ASSERT_TRUE_MESSAGE(IS_WITHIN_1PERC(_want_, _is_), "Not within margin.")
#define TEST_ASSERT_MUX_OFF()                                                                                          \
  do {                                                                                                                 \
    uint8_t cnfg_chg_i;                                                                                                \
    mock_read_regs(REG(CNFG_CHG_I), 1, &cnfg_chg_i);                                                                   \
    TEST_ASSERT_EQUAL_HEX(REG_CONST(CNFG_CHG_I, MUX_SEL, DISABLED), REG_FIELD_GET(CNFG_CHG_I, MUX_SEL, cnfg_chg_i));   \
  } while (0)

// ======== Tests ==================================================================================

void test_measure_chgin_v() {
  uint32_t result;

  mock_set_adc_params(0xFFF, 1250); // 12bit, 1.25V FS ADC

  // CHGIN_V maps 0V-7.5V to 0V-1.25V

  // 0V:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, CHGIN_V), 0);
  max77654_measure(&h, MAX77654_CHGIN_V, &result);
  TEST_ASSERT_EQUAL(0, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 7.5V (7.485 due rounding):
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, CHGIN_V), 1250);
  max77654_measure(&h, MAX77654_CHGIN_V, &result);
  TEST_ASSERT_WITHIN_1PERC(7500, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}

void test_measure_chgin_i() {
  uint32_t result;

  mock_set_adc_params(0xFFFF, 3300); // 16bit, 3.3V FS ADC

  // CHGIN_I maps 0A-0.475A to 0V-1.25V

  // 0A:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, CHGIN_I), 0);
  max77654_measure(&h, MAX77654_CHGIN_I, &result);
  TEST_ASSERT_EQUAL(0, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 0.475A
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, CHGIN_I), 1250);
  max77654_measure(&h, MAX77654_CHGIN_I, &result);
  TEST_ASSERT_WITHIN_1PERC(475, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}

void test_measure_batt_v() {
  uint32_t result;

  mock_set_adc_params(0xFFFF, 3000); // 16bit, 3V FS ADC

  // BATT_V maps 0V-4.6A to 0V-1.25V

  // 0A:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_V), 0);
  max77654_measure(&h, MAX77654_BATT_V, &result);
  TEST_ASSERT_EQUAL(0, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 4.6V
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_V), 1250);
  max77654_measure(&h, MAX77654_BATT_V, &result);
  TEST_ASSERT_WITHIN_1PERC(4600, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}
void test_measure_batt_i_chg() {
  uint32_t result;

  mock_set_adc_params(0xFFFF, 1800); // 16bit, 1.8V FS ADC

  // BATT_CHG_I maps 0%-100% to 0V - 1.25V

  // 0%:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_CHG_I), 0);
  max77654_measure(&h, MAX77654_BATT_I_CHG, &result);
  TEST_ASSERT_EQUAL(0, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 100%:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_CHG_I), 1250);
  max77654_measure(&h, MAX77654_BATT_I_CHG, &result);
  TEST_ASSERT_WITHIN_1PERC(100, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}

void test_measure_vsys() {
  uint32_t result;

  mock_set_adc_params(0xFFFF, 1800); // 16bit, 1.8V FS ADC

  // VSYS maps 0V - 4.8V to 0V - 1.25V

  // 0V:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, VSYS), 0);
  max77654_measure(&h, MAX77654_VSYS, &result);
  TEST_ASSERT_EQUAL(0, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 4.8V:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, VSYS), 1250);
  max77654_measure(&h, MAX77654_VSYS, &result);
  TEST_ASSERT_WITHIN_1PERC(4800, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}

void test_measure_batt_i() {
  uint32_t result;

  mock_set_adc_params(0xFFFF, 1800); // 16bit, 1.8V FS ADC

  max77654_measure_t batt_i_measurement_ranges[11] = {
      MAX77654_BATT_I_8MA2,   MAX77654_BATT_I_40MA5,  MAX77654_BATT_I_72MA3,  MAX77654_BATT_I_103MA4,
      MAX77654_BATT_I_134MA1, MAX77654_BATT_I_164MA1, MAX77654_BATT_I_193MA7, MAX77654_BATT_I_222MA7,
      MAX77654_BATT_I_251MA2, MAX77654_BATT_I_279MA3, MAX77654_BATT_I_300MA,
  };

  // First, test full scale with Vnull = 0V
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 0);

  // With Vnull=0, BTT_I maps 0% - 100% to 0V - 1.25V

  // 0%
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 0);
  for (unsigned int i = 0; i < 11; i++) {
    max77654_measure(&h, batt_i_measurement_ranges[i], &result);
    TEST_ASSERT_EQUAL(0, result);

    // Make sure the mux was returned to the 'off' position:
    TEST_ASSERT_MUX_OFF();
  }

  // 100%:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 1250);
  for (unsigned int i = 0; i < 11; i++) {
    max77654_measure(&h, batt_i_measurement_ranges[i], &result);
    TEST_ASSERT_WITHIN_1PERC(100, result);

    // Make sure the mux was returned to the 'off' position:
    TEST_ASSERT_MUX_OFF();
  }

  // now, test with Vnull = 100mV
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 100);

  // batt_i should now be 0% at 100mV:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 100);
  for (unsigned int i = 0; i < 11; i++) {
    max77654_measure(&h, batt_i_measurement_ranges[i], &result);
    TEST_ASSERT_EQUAL(0, result);

    // Make sure the mux was returned to the 'off' position:
    TEST_ASSERT_MUX_OFF();
  }

  // and still be 100% at 1.25V:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 1250);
  for (unsigned int i = 0; i < 11; i++) {
    max77654_measure(&h, batt_i_measurement_ranges[i], &result);
    TEST_ASSERT_WITHIN_1PERC(100, result);

    // Make sure the mux was returned to the 'off' position:
    TEST_ASSERT_MUX_OFF();
  }

  // Make sure batt_i < batt_i_null does not cause a problem:
  // Set BATT_I_NULL to 100mV, but BATT_I to 0mV:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 100);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 0);
  for (unsigned int i = 0; i < 11; i++) {
    max77654_measure(&h, batt_i_measurement_ranges[i], &result);
    TEST_ASSERT_EQUAL(0, result);

    // Make sure the mux was returned to the 'off' position:
    TEST_ASSERT_MUX_OFF();
  }
}

// All the measurements that are 1:1
void test_measure_identity() {
  uint32_t result;
  mock_set_adc_params(0xFFFF, 1800); // 16bit, 1.8V FS ADC

  // 0:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 0);
  max77654_measure(&h, MAX77654_BATT_I_NULL, &result);
  TEST_ASSERT_EQUAL(0, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, THM), 0);
  max77654_measure(&h, MAX77654_THM, &result);
  TEST_ASSERT_EQUAL(0, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, THM_BIAS), 0);
  max77654_measure(&h, MAX77654_TBIAS, &result);
  TEST_ASSERT_EQUAL(0, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, AGND), 0);
  max77654_measure(&h, MAX77654_AGND, &result);
  TEST_ASSERT_EQUAL(0, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  // 1.25V:
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 1250);
  max77654_measure(&h, MAX77654_BATT_I_NULL, &result);
  TEST_ASSERT_WITHIN_1PERC(1250, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, THM), 1250);
  max77654_measure(&h, MAX77654_THM, &result);
  TEST_ASSERT_WITHIN_1PERC(1250, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, THM_BIAS), 1250);
  max77654_measure(&h, MAX77654_TBIAS, &result);
  TEST_ASSERT_WITHIN_1PERC(1250, result);
  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();

  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, AGND), 1250); // if AGND ever measures 1.25V, panik.
  max77654_measure(&h, MAX77654_AGND, &result);
  TEST_ASSERT_WITHIN_1PERC(1250, result);

  // Make sure the mux was returned to the 'off' position:
  TEST_ASSERT_MUX_OFF();
}

// ---- max77654_measure_dischg_ua ----

static int fast_calls;
static uint8_t fast_mux[4];
static int plain_calls;

static max77654_err_t counting_adc_read_fast(struct max77654_adc_reading *reading) {
  uint8_t cnfg_chg_i = 0;
  mock_read_regs(REG(CNFG_CHG_I), 1, &cnfg_chg_i);
  if (fast_calls < 4)
    fast_mux[fast_calls] = REG_FIELD_GET(CNFG_CHG_I, MUX_SEL, cnfg_chg_i);
  fast_calls++;
  return mock_adc_read(reading);
}

static max77654_err_t counting_adc_read(struct max77654_adc_reading *reading) {
  plain_calls++;
  return mock_adc_read(reading);
}

void test_measure_dischg_ua() {
  uint32_t ua = 99;

  mock_set_adc_params(4095, 3600); // the SAADC of the board: 12 bit, 3.6 V full scale

  // Vnull = 50 mV, V_amux = 300 mV, 8.2 mA scale: (300 - 50) / (1250 - 50) x 8200 uA = 1708 uA
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 50);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 300);
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_UINT32_WITHIN(15, 1708, ua);
  TEST_ASSERT_MUX_OFF();

  // Full scale of each range with Vnull = 0
  const uint32_t scale_ua[11] = {8200, 40500, 72300, 103400, 134100, 164100, 193700, 222700, 251200, 279300, 300000};
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 0);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 1250);
  for (unsigned int i = 0; i < 11; i++) {
    TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, (max77654_measure_t)(MAX77654_BATT_I_8MA2 + i), &ua));
    TEST_ASSERT_UINT32_WITHIN(scale_ua[i] / 100 + 1, scale_ua[i], ua);
    TEST_ASSERT_MUX_OFF();
  }

  // The resolution is the ADC step, not a percent: 100 uA at the 8.2 mA scale is 1.5 mV of 1250 mV (2 steps of 0.88 mV)
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 15);
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_UINT32_WITHIN(6, 100, ua);
  // the integer percent function cannot see it
  uint32_t pct = 99;
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure(&h, MAX77654_BATT_I_8MA2, &pct));
  TEST_ASSERT_EQUAL(1, pct);

  // not above the null reading: 0 (noise), also with the same value
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 100);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 0);
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_EQUAL(0, ua);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 100);
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_EQUAL(0, ua);

  // Bad arguments
  TEST_ASSERT_NOT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_V, &ua));
  TEST_ASSERT_NOT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&h, MAX77654_BATT_I_8MA2, 0));

  // With adc_read_fast in the handle only that one is used, NULL first, then DISCHG, AMUX off afterwards
  struct max77654_h hf = h;
  hf.adc_read_fast = &counting_adc_read_fast;
  hf.adc_read = &counting_adc_read;
  fast_calls = plain_calls = 0;
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), 50);
  mock_set_adc_reading_voltage(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), 300);
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&hf, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_EQUAL(2, fast_calls);
  TEST_ASSERT_EQUAL(0, plain_calls);
  TEST_ASSERT_EQUAL_HEX(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I_NULL), fast_mux[0]);
  TEST_ASSERT_EQUAL_HEX(REG_CONST(CNFG_CHG_I, MUX_SEL, BATT_DISCHG_I), fast_mux[1]);
  TEST_ASSERT_MUX_OFF();
  // Without it adc_read is used
  hf.adc_read_fast = 0;
  fast_calls = plain_calls = 0;
  TEST_ASSERT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&hf, MAX77654_BATT_I_8MA2, &ua));
  TEST_ASSERT_EQUAL(2, plain_calls);
  // No ADC function at all
  hf.adc_read = 0;
  TEST_ASSERT_NOT_EQUAL(E_MAX77654_SUCCESS, max77654_measure_dischg_ua(&hf, MAX77654_BATT_I_8MA2, &ua));
}

// // ======== Main ===================================================================================

void setUp(void) {}
void tearDown(void) {}

int main(void) {
  h.read_regs = &mock_read_regs;
  h.write_regs = &mock_write_regs;
  h.adc_read = &mock_adc_read;

  UNITY_BEGIN();

  RUN_TEST(test_measure_chgin_v);
  RUN_TEST(test_measure_chgin_i);
  RUN_TEST(test_measure_batt_v);
  RUN_TEST(test_measure_batt_i_chg);
  RUN_TEST(test_measure_batt_i);
  RUN_TEST(test_measure_identity);
  RUN_TEST(test_measure_vsys);
  RUN_TEST(test_measure_dischg_ua);

  return UNITY_END();
}
