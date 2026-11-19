/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ts_ble.c
 * @brief Bluetooth side: anchor point reports, master sample sending, follower GATT service.
 *
 * Locking: plock (spinlock) guards the peer table. The anchor report path (Bluetooth RX thread) takes it for a table
 * lookup and a copy, never calls into GATT and does not allocate. Sending is done by a delayable work item on the
 * system work queue.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>

#include <bluetooth/hci_vs_sdc.h>

#include "ts_internal.h"

LOG_MODULE_REGISTER(ts_ble, CONFIG_SENSEI_TIMESYNC_LOG_LEVEL);

#define MASTER_ENABLED IS_ENABLED(CONFIG_SENSEI_TIMESYNC_MASTER)
#define FOLLOWER_ENABLED IS_ENABLED(CONFIG_SENSEI_TIMESYNC_FOLLOWER)

/* Vendor subevent: conn_handle (2), event_counter (2), anchor_point_us (8), after the subevent code. */
#define ANCHOR_REPORT_LEN (1 + 2 + 2 + 8)

enum disc_stage { DISC_NONE, DISC_SERVICE, DISC_CHRC, DISC_READY, DISC_FAILED };

struct peer {
  bool used;
  struct bt_conn *conn;
  uint16_t handle;
  bool central; /* we are the central on this connection */
  uint32_t interval_us;
  /* central side (master) */
  bool attached;
  enum disc_stage stage;
  uint16_t chrc_handle;
  bool anchor_valid;
  uint16_t anchor_counter;
  uint64_t anchor_us;
  bool send_pending; /* send as soon as a fresh anchor is known (after a parameter update) */
  struct bt_gatt_discover_params disc;
  /* peripheral side (follower) */
  bool is_master;
};

static struct peer peers[CONFIG_BT_MAX_CONN];
static struct k_spinlock plock;
static bool ble_ready;

#if MASTER_ENABLED
static uint32_t epoch;
static uint32_t epoch_mirror;
static struct k_work_delayable send_work;
static uint32_t send_errors;
#endif

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(SENSEI_TS_SVC_UUID_VAL);
static const struct bt_uuid_128 sample_uuid = BT_UUID_INIT_128(SENSEI_TS_SAMPLE_UUID_VAL);

/* plock held. */
static struct peer *peer_by_conn(const struct bt_conn *conn) {
  for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
    if (peers[i].used && peers[i].conn == conn) {
      return &peers[i];
    }
  }
  return NULL;
}

/* plock held. */
static struct peer *peer_by_handle(uint16_t handle) {
  for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
    if (peers[i].used && peers[i].handle == handle) {
      return &peers[i];
    }
  }
  return NULL;
}

/* Find or create the entry of conn. Must not be called with plock held (calls into the host). */
static struct peer *peer_get(struct bt_conn *conn) {
  struct bt_conn_info info;
  uint16_t handle;
  struct peer *p = NULL;

  if (bt_conn_get_info(conn, &info) != 0 || info.type != BT_CONN_TYPE_LE ||
      bt_hci_get_conn_handle(conn, &handle) != 0) {
    return NULL;
  }
  K_SPINLOCK(&plock) {
    p = peer_by_conn(conn);
    if (!p) {
      for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        if (!peers[i].used) {
          p = &peers[i];
          memset(p, 0, sizeof(*p));
          p->used = true;
          p->conn = conn;
          break;
        }
      }
    }
    if (p) {
      p->handle = handle;
      p->central = (info.role == BT_CONN_ROLE_CENTRAL);
      p->interval_us = info.le.interval_us;
    }
  }
  return p;
}

/* ---- Anchor point reports ---- */

static bool on_vs_evt(struct net_buf_simple *buf) {
  const uint8_t *d = buf->data;

  if (buf->len < ANCHOR_REPORT_LEN || d[0] != SDC_HCI_SUBEVENT_VS_CONN_ANCHOR_POINT_UPDATE_REPORT) {
    return false;
  }
  const uint16_t handle = sys_get_le16(&d[1]);
  const uint16_t counter = sys_get_le16(&d[3]);
  const uint64_t anchor_us = sys_get_le64(&d[5]);
  bool follow = false;
  bool kick = false;

  K_SPINLOCK(&plock) {
    struct peer *p = peer_by_handle(handle);

    if (p && p->central) {
      if (p->attached) {
        p->anchor_counter = counter;
        p->anchor_us = anchor_us;
        p->anchor_valid = true;
        kick = p->send_pending;
      }
    } else if (p) {
      /* Peripheral: the master connection is the one that wrote samples; before the first sample, the only
       * peripheral connection. */
      if (p->is_master) {
        follow = true;
      } else {
        int n = 0;
        bool other_master = false;

        for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
          if (peers[i].used && !peers[i].central) {
            n++;
            other_master |= peers[i].is_master;
          }
        }
        follow = (n == 1) && !other_master;
      }
    }
  }
#if MASTER_ENABLED
  if (kick) {
    k_work_reschedule(&send_work, K_NO_WAIT);
  }
#else
  ARG_UNUSED(kick);
#endif
  if (follow && FOLLOWER_ENABLED) {
    ts_core_follower_anchor(counter, anchor_us);
  }
  return true;
}

/* ---- Connection events ---- */

static void conn_connected(struct bt_conn *conn, uint8_t err) {
  if (err == 0) {
    (void)peer_get(conn);
  }
}

static void conn_disconnected(struct bt_conn *conn, uint8_t reason) {
  bool was_attached = false;
  bool was_master = false;

  ARG_UNUSED(reason);
  K_SPINLOCK(&plock) {
    struct peer *p = peer_by_conn(conn);

    if (p) {
      was_attached = p->attached;
      was_master = p->is_master;
      p->used = false;
    }
  }
  if (was_attached) {
    bt_conn_unref(conn);
  }
  if (was_master && FOLLOWER_ENABLED) {
    ts_core_follower_link_lost();
  }
}

static void conn_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
  bool follow = false;

  ARG_UNUSED(latency);
  ARG_UNUSED(timeout);
  K_SPINLOCK(&plock) {
    struct peer *p = peer_by_conn(conn);

    if (p) {
      p->interval_us = (uint32_t)interval * 1250u;
      if (p->central) {
        /* Anchors from before the update instant must not go out with the new interval. */
        p->anchor_valid = false;
        p->send_pending = true;
      } else if (p->is_master) {
        follow = true;
      }
    }
  }
  if (follow && FOLLOWER_ENABLED) {
    ts_core_follower_param_update();
  }
}

BT_CONN_CB_DEFINE(ts_conn_cb) = {
    .connected = conn_connected,
    .disconnected = conn_disconnected,
    .le_param_updated = conn_param_updated,
};

/* ---- Master ---- */

#if MASTER_ENABLED

static void new_epoch(void) {
  do {
    epoch = sys_rand32_get();
  } while (epoch == 0);
  epoch_mirror = ts_tb_mirror_starts();
}

static void send_samples(struct k_work *work) {
  struct ts_msg m = {.version = TS_MSG_VERSION, .epoch = epoch};
  uint8_t buf[TS_MSG_LEN];

  ARG_UNUSED(work);
  if (ts_tb_mirror_starts() != epoch_mirror) {
    /* The local clock restarted: followers must reset their models, so announce a new master clock. */
    new_epoch();
    m.epoch = epoch;
  }
  for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
    struct bt_conn *conn = NULL;
    uint16_t chrc = 0;

    K_SPINLOCK(&plock) {
      struct peer *p = &peers[i];

      if (p->used && p->attached && p->stage == DISC_READY && p->anchor_valid) {
        conn = bt_conn_ref(p->conn);
        chrc = p->chrc_handle;
        m.counter = p->anchor_counter;
        m.anchor_us = p->anchor_us;
        m.interval_us = p->interval_us;
        p->send_pending = false;
      }
    }
    if (!conn) {
      continue;
    }
    if (ts_msg_encode(&m, buf, sizeof(buf)) == TS_MSG_LEN) {
      /* Write command: no response. It may wait for an ATT buffer, so it runs on the work queue. */
      int err = bt_gatt_write_without_response(conn, chrc, buf, TS_MSG_LEN, false);

      if (err) {
        send_errors++;
        LOG_DBG("sample write: %d", err);
      }
    }
    bt_conn_unref(conn);
  }
  k_work_reschedule(&send_work, K_MSEC(CONFIG_SENSEI_TIMESYNC_SAMPLE_PERIOD_MS));
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
  struct peer *p = CONTAINER_OF(params, struct peer, disc);

  if (!attr) {
    LOG_WRN("time sync %s not found", p->stage == DISC_SERVICE ? "service" : "characteristic");
    p->stage = DISC_FAILED;
    return BT_GATT_ITER_STOP;
  }
  if (p->stage == DISC_SERVICE) {
    const struct bt_gatt_service_val *svc = attr->user_data;

    p->disc.uuid = &sample_uuid.uuid;
    p->disc.start_handle = attr->handle + 1;
    p->disc.end_handle = svc->end_handle;
    p->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    p->stage = DISC_CHRC;
    if (bt_gatt_discover(conn, &p->disc) != 0) {
      p->stage = DISC_FAILED;
    }
    return BT_GATT_ITER_STOP;
  }
  const struct bt_gatt_chrc *chrc = attr->user_data;

  K_SPINLOCK(&plock) { p->chrc_handle = chrc->value_handle; }
  p->stage = DISC_READY;
  LOG_INF("time sync characteristic at handle %u", chrc->value_handle);
  return BT_GATT_ITER_STOP;
}

int sensei_ts_master_attach(struct bt_conn *conn) {
  struct peer *p;
  int err;

  if (!conn) {
    return -EINVAL;
  }
  p = peer_get(conn);
  if (!p) {
    return -EINVAL;
  }
  int rc = 0;

  K_SPINLOCK(&plock) {
    size_t n = 0;

    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
      n += (peers[i].used && peers[i].attached);
    }
    if (!p->central) {
      rc = -EINVAL;
    } else if (p->attached) {
      rc = -EALREADY;
    } else if (n >= CONFIG_SENSEI_TIMESYNC_MAX_FOLLOWERS) {
      rc = -ENOMEM;
    } else {
      p->attached = true;
      p->stage = DISC_SERVICE;
      p->chrc_handle = 0;
      p->anchor_valid = false;
      p->send_pending = true;
    }
  }
  if (rc) {
    return rc;
  }
  bt_conn_ref(conn);
  memset(&p->disc, 0, sizeof(p->disc));
  p->disc.uuid = &svc_uuid.uuid;
  p->disc.func = discover_cb;
  p->disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  p->disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  p->disc.type = BT_GATT_DISCOVER_PRIMARY;
  err = bt_gatt_discover(conn, &p->disc);
  if (err) {
    sensei_ts_master_detach(conn);
    return err;
  }
  return 0;
}

void sensei_ts_master_detach(struct bt_conn *conn) {
  bool was = false;

  K_SPINLOCK(&plock) {
    struct peer *p = peer_by_conn(conn);

    if (p && p->attached) {
      p->attached = false;
      p->stage = DISC_NONE;
      p->anchor_valid = false;
      was = true;
    }
  }
  if (was) {
    bt_conn_unref(conn);
  }
}

#else /* !MASTER_ENABLED */

int sensei_ts_master_attach(struct bt_conn *conn) {
  ARG_UNUSED(conn);
  return -ENOTSUP;
}

void sensei_ts_master_detach(struct bt_conn *conn) { ARG_UNUSED(conn); }

#endif

/* ---- Follower ---- */

#if FOLLOWER_ENABLED

static ssize_t sample_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags) {
  struct ts_msg m;

  ARG_UNUSED(attr);
  ARG_UNUSED(flags);
  if (offset != 0) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }
  if (ts_msg_decode(&m, buf, len) != 0) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }
  struct peer *p = peer_get(conn);

  if (!p || p->central) {
    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
  }
  K_SPINLOCK(&plock) { p->is_master = true; }
  ts_core_follower_sample(&m);
  return len;
}

BT_GATT_SERVICE_DEFINE(ts_service, BT_GATT_PRIMARY_SERVICE(&svc_uuid),
                       BT_GATT_CHARACTERISTIC(&sample_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE,
                                              NULL, sample_write, NULL));

#endif

/* ---- Init ---- */

int sensei_ts_ble_init(void) {
  int err;

  if (ts_tb_net_started_early()) {
    LOG_ERR("network core started before the time sync init");
    return -EIO;
  }
  if (!sensei_ts_local_running()) {
    LOG_ERR("network core did not trigger the clock mirror: check ipc_radio (MPSL_TRIGGER_IPC_TASK_ON_RTC_START, "
            "channel %d)",
            CONFIG_SENSEI_TIMESYNC_IPC_CHANNEL);
    return -ENODEV;
  }
  err = bt_hci_register_vnd_evt_cb(on_vs_evt);
  if (err) {
    return err;
  }
  const sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t en = {.enable = 1};

  err = hci_vs_sdc_conn_anchor_point_update_event_report_enable(&en);
  if (err) {
    LOG_ERR("anchor point reports: %d (CONFIG_BT_CTLR_SDC_CONN_ANCHOR_POINT_REPORT in ipc_radio?)", err);
    return err;
  }
#if MASTER_ENABLED
  if (!ble_ready) {
    new_epoch();
    k_work_init_delayable(&send_work, send_samples);
    k_work_reschedule(&send_work, K_MSEC(CONFIG_SENSEI_TIMESYNC_SAMPLE_PERIOD_MS));
  }
#endif
  ble_ready = true;
  ts_pps_poke();
  return 0;
}
