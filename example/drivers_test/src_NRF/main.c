/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Probes every IC the SDK has a driver for, prints one reading and puts the IC into its lowest-power
 * state. Each test only runs if its devicetree node exists, so the same file serves the base board,
 * the sensor shield and the LPWAN shield. Untested on hardware.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include <sensei/drivers/gnss_m10s.h>
#include <sensei/drivers/lis2duxs12_extra.h>
#include <sensei/drivers/psram_aps12804o.h>

#include "pwr/pwr.h"

LOG_MODULE_REGISTER(drivers_test, LOG_LEVEL_INF);

/* How long to wait for a GNSS fix. Indoors there will be none, the test still checks the module. */
#define GNSS_WAIT_S 30

#define HAVE(label) DT_NODE_HAS_STATUS(DT_NODELABEL(label), okay)

/* The ST drivers come with the hal_st module. Without it the nodes stay but the drivers are not built. */
#define HAVE_LIS2DUXS12 (HAVE(lis2duxs12) && IS_ENABLED(CONFIG_LIS2DUX12))
#define HAVE_ISM330DHCX (HAVE(ism330dhcx) && IS_ENABLED(CONFIG_ISM330DHCX))
#define HAVE_ILPS28QSW (HAVE(ilps28qsw) && IS_ENABLED(CONFIG_ILPS28QSW_SENSEI))
#define HAVE_GNSS (HAVE(gnss) && IS_ENABLED(CONFIG_GNSS_M10S))
#define HAVE_PSRAM (HAVE(psram) && IS_ENABLED(CONFIG_PSRAM_APS12804O))

static int failures;

static void result(const char *ic, int ret, const char *detail) {
  if (ret == 0) {
    printk("[ OK ] %-12s %s\n", ic, detail);
  } else {
    printk("[FAIL] %-12s %s (%d)\n", ic, detail, ret);
    failures++;
  }
}

static void skip(const char *ic, const char *why) { printk("[ -- ] %-12s %s\n", ic, why); }

/* ---- PMIC ----------------------------------------------------------------------------------- */

static void pmic_event(uint32_t events, const struct pwr_status *st, void *user) {
  printk("PMIC event 0x%02x: %u mV, charger %d, input %d\n", events, st->bat_mv, st->charger, st->chgin);
}

static void test_pmic(void) {
  struct pwr_status st;
  char buf[96];
  int ret;

  pwr_set_event_cb(pmic_event, NULL);
  pwr_refresh();
  k_msleep(500);
  ret = pwr_get_status(&st);
  snprintf(buf, sizeof(buf), "battery %u mV (%u %%), charger state %d, input %d, charging %d", st.bat_mv, st.bat_perc,
           st.charger, st.chgin, st.charging);
  result("MAX77654", ret, buf);
  /* The PMIC stays on: it supplies everything else. */
}

/* ---- Base board ----------------------------------------------------------------------------- */

#if HAVE(led_0)
static void test_led(void) {
  const struct device *led = DEVICE_DT_GET(DT_NODELABEL(led_0));
  uint8_t blue[3] = {0, 0, 40};
  int ret = -ENODEV;

  if (device_is_ready(led)) {
    ret = led_set_color(led, 0, 3, blue);
    k_msleep(500);
    led_off(led, 0);
  }
  result("IS31FL3194", ret, "blue for 0.5 s, then off");
  /* Lowest power: LED_EN (SDB, P0.22) low. pwr_start() had set it high. */
  gpio_pin_set_dt(&(const struct gpio_dt_spec)GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_led_pwr), gpios), 0);
}
#endif

static void print_accel(const char *tag, const struct sensor_value *v, char *buf, size_t n) {
  snprintf(buf, n, "%s %.2f %.2f %.2f m/s2", tag, sensor_value_to_double(&v[0]), sensor_value_to_double(&v[1]),
           sensor_value_to_double(&v[2]));
}

static int probe_who_am_i(const struct device *bus, uint8_t addr, uint8_t reg, uint8_t expect) {
  uint8_t id = 0;

  return (i2c_reg_read_byte(bus, addr, reg, &id) == 0 && id == expect) ? 0 : -ENODEV;
}

#if HAVE_LIS2DUXS12
static void test_lis2duxs12(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lis2duxs12));
  static const struct i2c_dt_spec spec = I2C_DT_SPEC_GET(DT_NODELABEL(lis2duxs12));
  struct sensor_value odr = {.val1 = 25};
  struct sensor_value v[3];
  char buf[96] = "";
  int ret = -ENODEV;

  if (device_is_ready(dev)) {
    ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    k_msleep(200);
    if (ret == 0) {
      ret = sensor_sample_fetch(dev);
    }
    if (ret == 0) {
      ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v);
    }
    odr.val1 = 0;
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    if (ret == 0) {
      print_accel("accel", v, buf, sizeof(buf));
    }
    /* Lowest power: deep power-down. */
    if (sensei_lis2duxs12_deep_power_down(&spec) != 0) {
      LOG_WRN("LIS2DUXS12 deep power-down failed");
    }
  } else {
    /* The address depends on SA0 (not tied on the base board): see which one answers. */
    const struct device *bus = spec.bus;
    snprintf(buf, sizeof(buf), "not ready, WHO_AM_I at 0x18: %s, at 0x19: %s",
             probe_who_am_i(bus, 0x18, 0x0F, 0x47) ? "no" : "yes",
             probe_who_am_i(bus, 0x19, 0x0F, 0x47) ? "no" : "yes");
  }
  result("LIS2DUXS12", ret, buf);
}
#endif

/* ---- Sensor shield -------------------------------------------------------------------------- */

#if HAVE_ISM330DHCX
static void test_ism330dhcx(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ism330dhcx));
  struct sensor_value odr = {.val1 = 26};
  struct sensor_value a[3], g[3];
  char buf[128] = "";
  int ret = -ENODEV;

  if (device_is_ready(dev)) {
    ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    if (ret == 0) {
      ret = sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    }
    k_msleep(200);
    if (ret == 0) {
      ret = sensor_sample_fetch(dev);
    }
    if (ret == 0) {
      sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, a);
      sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, g);
      snprintf(buf, sizeof(buf), "accel %.2f %.2f %.2f m/s2, gyro %.3f %.3f %.3f rad/s", sensor_value_to_double(&a[0]),
               sensor_value_to_double(&a[1]), sensor_value_to_double(&a[2]), sensor_value_to_double(&g[0]),
               sensor_value_to_double(&g[1]), sensor_value_to_double(&g[2]));
    }
    /* Lowest power: both sensors powered down (odr 0). */
    odr.val1 = 0;
    sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    sensor_attr_set(dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
  } else {
    snprintf(buf, sizeof(buf), "not ready (WHO_AM_I at 0x6A: %s)",
             probe_who_am_i(DEVICE_DT_GET(DT_ALIAS(i2ca)), 0x6A, 0x0F, 0x6B) ? "no" : "yes");
  }
  result("ISM330DHCX", ret, buf);
}
#endif

#if HAVE_ILPS28QSW
static void test_ilps28qsw(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ilps28qsw));
  struct sensor_value p, t;
  char buf[64] = "";
  int ret = -ENODEV;

  if (device_is_ready(dev)) {
    ret = sensor_sample_fetch(dev);
    if (ret == 0) {
      sensor_channel_get(dev, SENSOR_CHAN_PRESS, &p);
      sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &t);
      snprintf(buf, sizeof(buf), "%.2f hPa, %.2f C (one-shot, powers down by itself)",
               sensor_value_to_double(&p) * 10.0, sensor_value_to_double(&t));
    }
  } else {
    snprintf(buf, sizeof(buf), "not ready");
  }
  result("ILPS28QSW", ret, buf);
}
#endif

#if HAVE(pca6416a_20)
static void test_pca6416(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(pca6416a_20));
  gpio_port_value_t in = 0;
  char buf[48];
  int ret = device_is_ready(dev) ? gpio_port_get_raw(dev, &in) : -ENODEV;

  snprintf(buf, sizeof(buf), "inputs 0x%04x (no low-power setting)", (unsigned)in);
  result("PCA6416A", ret, buf);
}
#endif

#if HAVE_GNSS
static K_SEM_DEFINE(gnss_sem, 0, 1);
static struct gnss_data gnss_last;
static uint16_t gnss_sats;

static void gnss_data_cb(const struct device *dev, const struct gnss_data *data) {
  gnss_last = *data;
  if (data->info.fix_status != GNSS_FIX_STATUS_NO_FIX) {
    k_sem_give(&gnss_sem);
  }
}
GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss)), gnss_data_cb);

#if CONFIG_GNSS_SATELLITES
static void gnss_sat_cb(const struct device *dev, const struct gnss_satellite *sats, uint16_t size) {
  gnss_sats = size;
}
GNSS_SATELLITES_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss)), gnss_sat_cb);
#endif

static void test_gnss(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(gnss));
  char buf[128] = "";
  int ret = -ENODEV;

  if (device_is_ready(dev)) {
    /* The node starts the module in standby: wake it, wait for the configuration and a fix. */
    ret = gnss_m10s_wake(dev);
    for (int i = 0; ret == 0 && i < GNSS_WAIT_S && k_sem_take(&gnss_sem, K_SECONDS(1)) != 0; i++) {
    }
    if (ret == 0 && !gnss_m10s_is_configured(dev)) {
      ret = -EIO;
      snprintf(buf, sizeof(buf), "no answer on I2C 0x42 or configuration rejected");
    } else if (ret == 0) {
      snprintf(buf, sizeof(buf), "fix status %d, %u satellites used, %u in view, time %02u:%02u",
               gnss_last.info.fix_status, gnss_last.info.satellites_cnt, gnss_sats, gnss_last.utc.hour,
               gnss_last.utc.minute);
    }
    /* Lowest power: software standby, wakes on EXTINT. */
    if (gnss_m10s_standby(dev) != 0) {
      LOG_WRN("GNSS standby failed");
    }
  } else {
    snprintf(buf, sizeof(buf), "not ready");
  }
  result("MAX-M10S", ret, buf);
}
#endif

/* ---- LPWAN shield --------------------------------------------------------------------------- */

#if HAVE(lpwan_expander)
static void test_tcal6408(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lpwan_expander));
  gpio_port_value_t in = 0;
  char buf[48];
  int ret = device_is_ready(dev) ? gpio_port_get_raw(dev, &in) : -ENODEV;

  snprintf(buf, sizeof(buf), "inputs 0x%02x (shield 3V3 and level shifter are on the expander)", (unsigned)in);
  result("TCAL6408", ret, buf);
}
#endif

/* ---- Base board, PSRAM and button ------------------------------------------------------------ */

#if HAVE_PSRAM
static void test_psram(void) {
  const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(psram));
  uint8_t out[37], in[37];
  const uint32_t addr = 0x1001; /* unaligned on purpose */
  int ret = psram_aps12804o_power_on(dev);

  for (size_t i = 0; i < sizeof(out); i++) {
    out[i] = (uint8_t)(i * 7 + 1);
  }
  if (ret == 0) {
    ret = psram_aps12804o_write(dev, addr, out, sizeof(out));
  }
  if (ret == 0) {
    /* Half-sleep keeps the data. */
    ret = psram_aps12804o_half_sleep(dev);
    k_msleep(1);
    ret = ret ? ret : psram_aps12804o_wake(dev);
  }
  if (ret == 0) {
    ret = psram_aps12804o_read(dev, addr, in, sizeof(in));
  }
  if (ret == 0 && memcmp(in, out, sizeof(in)) != 0) {
    ret = -EIO;
  }
  psram_aps12804o_power_off(dev);
  result("APS12804O", ret, "write, half-sleep, wake, read back, power off");
}
#endif

static void test_button(void) {
  static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios);
  char buf[32];
  int ret = gpio_pin_configure_dt(&btn, GPIO_INPUT);

  snprintf(buf, sizeof(buf), "nRF_btn %s", gpio_pin_get_dt(&btn) > 0 ? "pressed" : "released");
  result("Button", ret, buf);
}

int main(void) {
  LOG_INF("SENSEI driver test on %s", CONFIG_BOARD);

  if (pwr_init() < 0 || pwr_start() < 0) {
    LOG_ERR("power start failed, sensors have no supply");
  }
  if (usb_enable(NULL) == 0) {
    k_sleep(K_SECONDS(5)); /* time to open the USB console */
  }

  printk("\n--- SENSEI driver test ---\n");
  test_pmic();
#if HAVE(led_0)
  test_led();
#endif
#if HAVE_LIS2DUXS12
  test_lis2duxs12();
#else
  skip("LIS2DUXS12", "driver not built (hal_st module missing?)");
#endif
#if HAVE_ISM330DHCX
  test_ism330dhcx();
#else
  skip("ISM330DHCX", "driver not built (hal_st module missing?)");
#endif
#if HAVE_ILPS28QSW
  test_ilps28qsw();
#else
  skip("ILPS28QSW", "driver not built (needs CONFIG_SENSEI)");
#endif
#if HAVE(pca6416a_20)
  test_pca6416();
#endif
#if HAVE_GNSS
  test_gnss();
#else
  skip("MAX-M10S", "no node (sensor shield missing?)");
#endif
#if HAVE(lpwan_expander)
  test_tcal6408();
#endif
#if HAVE_PSRAM
  test_psram();
#else
  skip("APS12804O", "node disabled (enable &psram in an overlay)");
#endif
  skip("nRF21540", "not tested here (net core, needs the LPWAN shield, see the shield overlay)");
  test_button();

  printk("--- %d failure(s) ---\n", failures);
  while (1) {
    k_sleep(K_SECONDS(10));
  }
  return 0;
}
