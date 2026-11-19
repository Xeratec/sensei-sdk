/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file follower.c
 * @brief Follower role: advertise, report the global time of every captured edge to the master.
 */

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_DECLARE(app, CONFIG_APP_LOG_LEVEL);

static const struct bt_uuid_128 report_chr_uuid = BT_UUID_INIT_128(APP_REPORT_CHR_UUID_VAL);

K_MSGQ_DEFINE(report_q, sizeof(struct app_report), 8, 4);

static uint32_t edge_seq;
static uint32_t dropped;

/* ---- Report service ---- */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
  LOG_INF("Master %s edge reports", value == BT_GATT_CCC_NOTIFY ? "subscribed to" : "unsubscribed from");
}

BT_GATT_SERVICE_DEFINE(report_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(APP_REPORT_SVC_UUID_VAL)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(APP_REPORT_CHR_UUID_VAL), BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

/* ---- Advertising ---- */

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, SENSEI_TS_SVC_UUID_VAL),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise(void) {
  int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

  if (err && err != -EALREADY) {
    LOG_ERR("Advertising failed (%d)", err);
  } else {
    LOG_INF("Advertising the time sync service");
  }
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
  char str[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), str, sizeof(str));
  if (err) {
    LOG_ERR("Connection from %s failed (0x%02x)", str, err);
  } else {
    LOG_INF("Connected to master %s", str);
  }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) { LOG_INF("Disconnected (reason 0x%02x)", reason); }

static void on_recycled(void) { advertise(); }

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
  LOG_INF("Connection interval %u.%02u ms, latency %u", interval * 5 / 4, (interval * 125) % 100, latency);
}

BT_CONN_CB_DEFINE(follower_conn_cb) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
    .le_param_updated = on_le_param_updated,
};

/* ---- Capture ---- */

static void capture_cb(uint8_t channel, uint64_t local_ns, void *user) {
  struct sensei_ts_time t;
  struct app_report r = {.seq = ++edge_seq};

  /* Convert here with the model of this moment. Edges without global time only advance the sequence number. */
  if (sensei_ts_local_to_global(local_ns, &t) != 0) {
    return;
  }
  r.global_ns = t.ns;
  r.uncert_ns = t.uncert_ns;
  r.state = t.state;
  if (k_msgq_put(&report_q, &r, K_NO_WAIT) != 0) {
    dropped++;
  }
}

void app_role_run(void) {
  struct app_report r;
  uint8_t buf[APP_REPORT_LEN];
  int err;

  err = sensei_ts_capture_start(0, &app_pin_capture, GPIO_INT_EDGE_RISING, capture_cb, NULL);
  if (err) {
    LOG_ERR("Capture start failed (%d)", err);
  }

  advertise();

  while (true) {
    k_msgq_get(&report_q, &r, K_FOREVER);
    app_report_pack(&r, buf);
    err = bt_gatt_notify_uuid(NULL, &report_chr_uuid.uuid, report_svc.attrs, buf, sizeof(buf));
    if (err) {
      LOG_WRN("Report %u not sent (%d)", r.seq, err);
    } else {
      LOG_DBG("Edge %u: global %llu ns, uncert %u ns, %s", r.seq, (unsigned long long)r.global_ns, r.uncert_ns,
              app_state_str(r.state));
    }
    if (dropped) {
      LOG_WRN("%u reports dropped (queue full)", dropped);
      dropped = 0;
    }
  }
}
