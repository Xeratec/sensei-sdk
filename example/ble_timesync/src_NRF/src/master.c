/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file master.c
 * @brief Master role: scan and connect to the followers, test pulse, own capture.
 */

#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "app.h"

LOG_MODULE_DECLARE(app, CONFIG_APP_LOG_LEVEL);

#define MAX_FOLLOWERS CONFIG_SENSEI_TIMESYNC_MAX_FOLLOWERS
#define CONN_TIMEOUT_10MS 400 /* supervision timeout 4 s */
#define DISCOVER_RETRY_MS 500
#define DISCOVER_TRIES 20
#define TEST_PULSE_WIDTH_MS 2

static const struct bt_uuid_128 ts_svc_uuid = BT_UUID_INIT_128(SENSEI_TS_SVC_UUID_VAL);
static const struct bt_uuid_128 report_chr_uuid = BT_UUID_INIT_128(APP_REPORT_CHR_UUID_VAL);

struct follower {
  struct bt_conn *conn;
  struct k_work_delayable discover_work;
  struct bt_gatt_discover_params discover;
  struct bt_gatt_subscribe_params subscribe;
  struct bt_gatt_discover_params ccc_discover;
  uint8_t tries;
};

static struct follower followers[CONFIG_BT_MAX_CONN];
static atomic_t n_followers;
static atomic_t connecting;

static void scan_start(void);

/* ---- Report subscription ---- */

static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params, const void *data,
                         uint16_t length) {
  struct app_report r;

  if (data == NULL) {
    LOG_INF("F%u: report unsubscribed", bt_conn_index(conn));
    return BT_GATT_ITER_STOP;
  }
  if (app_report_unpack(data, length, &r) != 0) {
    LOG_WRN("F%u: report of %u bytes ignored", bt_conn_index(conn), length);
    return BT_GATT_ITER_CONTINUE;
  }
  measure_report(bt_conn_index(conn), &r);
  return BT_GATT_ITER_CONTINUE;
}

static void on_subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params) {
  if (err) {
    LOG_ERR("F%u: subscribe failed (0x%02x)", bt_conn_index(conn), err);
  } else {
    LOG_INF("F%u: subscribed to edge reports", bt_conn_index(conn));
  }
}

static uint8_t on_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
  struct follower *f = &followers[bt_conn_index(conn)];
  int err;

  if (attr == NULL) {
    LOG_WRN("F%u: report characteristic not found", bt_conn_index(conn));
    return BT_GATT_ITER_STOP;
  }

  memset(&f->subscribe, 0, sizeof(f->subscribe));
  f->subscribe.notify = on_notify;
  f->subscribe.subscribe = on_subscribed;
  f->subscribe.value = BT_GATT_CCC_NOTIFY;
  f->subscribe.value_handle = bt_gatt_attr_value_handle(attr);
  /* ccc_handle 0: the host finds the CCC descriptor after the value (BT_GATT_AUTO_DISCOVER_CCC). */
  f->subscribe.ccc_handle = 0;
  f->subscribe.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  f->subscribe.disc_params = &f->ccc_discover;

  err = bt_gatt_subscribe(conn, &f->subscribe);
  if (err && err != -EALREADY) {
    LOG_ERR("F%u: bt_gatt_subscribe failed (%d)", bt_conn_index(conn), err);
  }
  return BT_GATT_ITER_STOP;
}

/* The library discovers its own service on the same connection. Try again while that is going on. */
static void discover_fn(struct k_work *work) {
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct follower *f = CONTAINER_OF(dwork, struct follower, discover_work);
  int err;

  if (f->conn == NULL) {
    return;
  }

  memset(&f->discover, 0, sizeof(f->discover));
  f->discover.uuid = &report_chr_uuid.uuid;
  f->discover.func = on_discover;
  f->discover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  f->discover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  f->discover.type = BT_GATT_DISCOVER_CHARACTERISTIC;

  err = bt_gatt_discover(f->conn, &f->discover);
  if (err) {
    if (++f->tries < DISCOVER_TRIES) {
      k_work_reschedule(dwork, K_MSEC(DISCOVER_RETRY_MS));
    } else {
      LOG_ERR("F%u: discovery failed (%d)", bt_conn_index(f->conn), err);
    }
  }
}

/* ---- Connections ---- */

static bool ad_has_ts_uuid(struct bt_data *data, void *user_data) {
  bool *found = user_data;

  if ((data->type == BT_DATA_UUID128_ALL || data->type == BT_DATA_UUID128_SOME) && data->data_len % 16 == 0) {
    for (uint8_t i = 0; i < data->data_len; i += 16) {
      if (memcmp(&data->data[i], ts_svc_uuid.val, 16) == 0) {
        *found = true;
        return false;
      }
    }
  }
  return true;
}

static void on_scan(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad) {
  struct bt_conn *conn = NULL;
  bool found = false;
  char str[BT_ADDR_LE_STR_LEN];
  int err;

  if (type != BT_GAP_ADV_TYPE_ADV_IND) {
    return;
  }
  bt_data_parse(ad, ad_has_ts_uuid, &found);
  if (!found || !atomic_cas(&connecting, 0, 1)) {
    return;
  }

  bt_addr_le_to_str(addr, str, sizeof(str));
  LOG_INF("Found follower %s (rssi %d), connecting", str, rssi);

  err = bt_le_scan_stop();
  if (err) {
    LOG_ERR("Scan stop failed (%d)", err);
    atomic_clear(&connecting);
    return;
  }

  struct bt_le_conn_param param =
      BT_LE_CONN_PARAM_INIT(BT_GAP_MS_TO_CONN_INTERVAL(CONFIG_APP_CONN_INTERVAL_MS),
                            BT_GAP_MS_TO_CONN_INTERVAL(CONFIG_APP_CONN_INTERVAL_MS), 0, CONN_TIMEOUT_10MS);

  err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, &param, &conn);
  if (err) {
    LOG_ERR("Connection create failed (%d)", err);
    atomic_clear(&connecting);
    scan_start();
    return;
  }
  /* The reference of bt_conn_le_create() is kept until the disconnection (connected() on error). */
}

static void scan_start(void) {
  int err;

  if (atomic_get(&n_followers) >= MAX_FOLLOWERS) {
    return;
  }
  err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, on_scan);
  if (err && err != -EALREADY) {
    LOG_ERR("Scan start failed (%d)", err);
  }
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
  uint8_t idx = bt_conn_index(conn);
  struct follower *f = &followers[idx];
  char str[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), str, sizeof(str));
  atomic_clear(&connecting);

  if (err) {
    LOG_ERR("Connection to %s failed (0x%02x)", str, err);
    bt_conn_unref(conn);
    scan_start();
    return;
  }

  LOG_INF("F%u connected: %s", idx, str);
  atomic_inc(&n_followers);

  memset(&f->subscribe, 0, sizeof(f->subscribe));
  f->conn = conn;
  f->tries = 0;
  measure_follower_add(idx, bt_conn_get_dst(conn));

  err = sensei_ts_master_attach(conn);
  if (err) {
    LOG_ERR("F%u: sensei_ts_master_attach failed (%d)", idx, err);
  }

  err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
  if (err) {
    LOG_WRN("F%u: PHY update failed (%d)", idx, err);
  }

  k_work_reschedule(&f->discover_work, K_MSEC(DISCOVER_RETRY_MS));
  scan_start();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
  uint8_t idx = bt_conn_index(conn);
  struct follower *f = &followers[idx];

  LOG_INF("F%u disconnected (reason 0x%02x)", idx, reason);
  k_work_cancel_delayable(&f->discover_work);
  measure_follower_remove(idx);
  f->conn = NULL;
  atomic_dec(&n_followers);
  bt_conn_unref(conn);
  scan_start();
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
  LOG_INF("F%u: connection interval %u.%02u ms, latency %u", bt_conn_index(conn), interval * 5 / 4,
          (interval * 125) % 100, latency);
}

static void on_le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param) {
  LOG_INF("F%u: PHY tx %u rx %u (2 = 2M)", bt_conn_index(conn), param->tx_phy, param->rx_phy);
}

BT_CONN_CB_DEFINE(master_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .le_param_updated = on_le_param_updated,
    .le_phy_updated = on_le_phy_updated,
};

/* ---- Capture, test pulse, button ---- */

static void capture_cb(uint8_t channel, uint64_t local_ns, void *user) {
  struct sensei_ts_time t;

  /* Global time == local time on the master. */
  if (sensei_ts_local_to_global(local_ns, &t) == 0) {
    measure_capture_add(t.ns);
  }
}

static void print_work_fn(struct k_work *work) { measure_print_all(); }

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0))
static K_WORK_DEFINE(print_work, print_work_fn);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
  k_work_submit(&print_work);
}

static void button_init(void) {
  if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) != 0) {
    return;
  }
  gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
  gpio_add_callback(button.port, &button_cb);
  gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
  LOG_INF("Button sw0: print and restart the statistics");
}
#else
static void button_init(void) {}
#endif

static void test_pulse(void) {
  uint32_t wait_ms = CONFIG_APP_TEST_PULSE_BASE_MS + sys_rand32_get() % CONFIG_APP_TEST_PULSE_JITTER_MS;

  k_msleep(wait_ms);
  gpio_pin_set_dt(&app_pin_test_out, 1);
  k_msleep(TEST_PULSE_WIDTH_MS);
  gpio_pin_set_dt(&app_pin_test_out, 0);
}

void app_role_run(void) {
  int err;

  for (size_t i = 0; i < ARRAY_SIZE(followers); i++) {
    k_work_init_delayable(&followers[i].discover_work, discover_fn);
  }

  err = sensei_ts_capture_start(0, &app_pin_capture, GPIO_INT_EDGE_RISING, capture_cb, NULL);
  if (err) {
    LOG_ERR("Capture start failed (%d)", err);
  }

  if (IS_ENABLED(CONFIG_APP_TEST_PULSE)) {
    err = gpio_pin_configure_dt(&app_pin_test_out, GPIO_OUTPUT_INACTIVE);
    if (err) {
      LOG_ERR("Test pulse pin failed (%d)", err);
    }
  }

  button_init();
  scan_start();

  while (true) {
    if (IS_ENABLED(CONFIG_APP_TEST_PULSE)) {
      test_pulse();
    } else {
      k_sleep(K_FOREVER);
    }
  }
}
