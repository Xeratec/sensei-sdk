/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * u-blox MAX-M10S on I2C (DDC). UBX protocol only.
 *
 * Read: register 0xFD/0xFE hold the number of waiting bytes, register 0xFF is the byte stream.
 * Write: UBX frames go to the I2C address without a register.
 * Untested on hardware.
 */

#define DT_DRV_COMPAT u_blox_max_m10s

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gnss/gnss_publish.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <sensei/drivers/gnss_m10s.h>

#include "ubx.h"

LOG_MODULE_REGISTER(gnss_m10s, CONFIG_GNSS_LOG_LEVEL);

#define M10S_REG_AVAIL 0xFD
#define M10S_REG_STREAM 0xFF
#define M10S_CHUNK 64
#define M10S_TX_SIZE 128
#define M10S_BOOT_MS 500
#define M10S_RETRY_MS 5000
#define M10S_BUSY_RETRY_MS 100 /* bus owned by someone else: look again soon */
#define M10S_ACK_TIMEOUT_MS 600
#define M10S_WAKE_PULSE_MS 20
#define M10S_STANDBY_TRIES 10
/* Layers written by the driver: RAM for now, BBR so the settings survive standby. */
#define M10S_LAYERS (UBX_LAYER_RAM | UBX_LAYER_BBR)
/* Systems the M10 can track (not all at once: the receiver rejects too many). */
#define M10S_SYSTEMS_SUPPORTED                                                                                         \
  (GNSS_SYSTEM_GPS | GNSS_SYSTEM_GLONASS | GNSS_SYSTEM_GALILEO | GNSS_SYSTEM_BEIDOU | GNSS_SYSTEM_QZSS |               \
   GNSS_SYSTEM_SBAS)
/* Receiver defaults. */
#define M10S_SYSTEMS_DEFAULT (GNSS_SYSTEM_GPS | GNSS_SYSTEM_GALILEO | GNSS_SYSTEM_QZSS | GNSS_SYSTEM_SBAS)

struct m10s_config {
  struct i2c_dt_spec i2c;
  struct gpio_dt_spec reset;
  struct gpio_dt_spec extint;
  struct gpio_dt_spec timepulse;
  uint32_t fix_interval_ms;
  uint32_t poll_interval_ms;
  uint32_t psm_period_s;
  uint8_t psm_mode;
  bool polled;
  bool standby_at_boot;
};

struct m10s_data {
  const struct device *dev;
  struct k_work_delayable work;
  struct k_mutex lock;
  struct ubx_parser parser;
  uint8_t rx[CONFIG_GNSS_M10S_RX_BUFFER_SIZE];
  uint8_t chunk[M10S_CHUNK];
  uint8_t tx[M10S_TX_SIZE];
#if CONFIG_GNSS_SATELLITES
  struct gnss_satellite sats[CONFIG_GNSS_M10S_MAX_SATELLITES];
#endif
  uint32_t fix_interval_ms;
  uint32_t psm_period_s;
  uint8_t psm_mode;
  enum gnss_navigation_mode nav_mode;
  gnss_systems_t systems;
  bool systems_set;
  struct gnss_m10s_pvt_ext pvt_ext; /* extras of the last NAV-PVT, under pvt_lock */
  struct k_spinlock pvt_lock;
  bool active;     /* not in standby */
  bool configured; /* module answered and took the configuration */
  int (*bus_lock)(void *ctx);
  void (*bus_unlock)(void *ctx);
  void *bus_ctx;
  bool polled;
  bool poll_wait; /* polled mode: requests sent, answer not read yet */
  uint8_t standby_tries;
  uint8_t ack_cls;
  uint8_t ack_id;
  int8_t ack_state; /* -1 waiting, 0 NAK, 1 ACK */
};

/* ---- I2C stream ------------------------------------------------------------------------- */

static int m10s_avail(const struct device *dev, uint16_t *avail) {
  const struct m10s_config *cfg = dev->config;
  uint8_t b[2];
  int ret = i2c_burst_read_dt(&cfg->i2c, M10S_REG_AVAIL, b, sizeof(b));

  if (ret == 0) {
    *avail = (uint16_t)((b[0] << 8) | b[1]);
  }
  return ret;
}

static int m10s_write(const struct device *dev, const uint8_t *buf, size_t len) {
  const struct m10s_config *cfg = dev->config;

  return i2c_write_dt(&cfg->i2c, buf, len);
}

static void m10s_publish_pvt(const struct device *dev, const struct ubx_nav_pvt *p) {
  struct m10s_data *mdata = dev->data;
  enum gnss_fix_status status = GNSS_FIX_STATUS_NO_FIX;
  enum gnss_fix_quality quality = GNSS_FIX_QUALITY_INVALID;

  if ((p->flags & UBX_PVT_FLAGS_GNSS_FIX_OK) && !(p->flags3 & UBX_PVT_FLAGS3_INVALID_LLH)) {
    switch (p->fix_type) {
    case UBX_FIX_2D:
    case UBX_FIX_3D:
      if (p->flags & UBX_PVT_FLAGS_DIFF_SOLN) {
        status = GNSS_FIX_STATUS_DGNSS_FIX;
        quality = GNSS_FIX_QUALITY_DGNSS;
      } else {
        status = GNSS_FIX_STATUS_GNSS_FIX;
        quality = GNSS_FIX_QUALITY_GNSS_SPS;
      }
      break;
    case UBX_FIX_DEAD_RECKONING:
    case UBX_FIX_GNSS_DR:
      status = GNSS_FIX_STATUS_ESTIMATED_FIX;
      quality = GNSS_FIX_QUALITY_ESTIMATED;
      break;
    default:
      break;
    }
  }

  int32_t heading_mdeg = p->head_mot_e5 / 100;
  if (heading_mdeg < 0) {
    heading_mdeg += 360000;
  }
  int32_t nano_ms = p->nano > 0 ? p->nano / 1000000 : 0;

  struct gnss_data data = {
      .info =
          {
              .satellites_cnt = p->num_sv,
              .hdop = (uint32_t)p->pdop_e2 * 10U, /* PDOP, the best the message has */
              .geoid_separation = p->height_mm - p->hmsl_mm,
              .fix_status = status,
              .fix_quality = quality,
          },
      .nav_data =
          {
              .latitude = (int64_t)p->lat_e7 * 100,
              .longitude = (int64_t)p->lon_e7 * 100,
              .bearing = (uint32_t)heading_mdeg,
              .speed = p->gspeed_mmps > 0 ? (uint32_t)p->gspeed_mmps : 0U,
              .altitude = p->hmsl_mm,
          },
      .utc =
          {
              .hour = p->hour,
              .minute = p->min,
              .millisecond = (uint16_t)(p->sec * 1000U + nano_ms),
              .month_day = p->day,
              .month = p->month,
              .century_year = (uint8_t)(p->year % 100U),
          },
  };

  /* Stored before the publish: inside a GNSS data callback the extras belong to this fix. */
  k_spinlock_key_t key = k_spin_lock(&mdata->pvt_lock);

  mdata->pvt_ext = (struct gnss_m10s_pvt_ext){
      .itow_ms = p->itow_ms,
      .h_acc_mm = p->h_acc_mm,
      .v_acc_mm = p->v_acc_mm,
      .height_mm = p->height_mm,
      .p_dop_e2 = p->pdop_e2,
      .fix_type = p->fix_type,
      .num_sv = p->num_sv,
      .fix_ok = (p->flags & UBX_PVT_FLAGS_GNSS_FIX_OK) && !(p->flags3 & UBX_PVT_FLAGS3_INVALID_LLH),
      .valid = true,
  };
  k_spin_unlock(&mdata->pvt_lock, key);

  gnss_publish_data(dev, &data);
}

#if CONFIG_GNSS_SATELLITES
static uint32_t sat_system(uint8_t gnss_id) {
  switch (gnss_id) {
  case UBX_GNSS_GPS:
    return GNSS_SYSTEM_GPS;
  case UBX_GNSS_SBAS:
    return GNSS_SYSTEM_SBAS;
  case UBX_GNSS_GALILEO:
    return GNSS_SYSTEM_GALILEO;
  case UBX_GNSS_BEIDOU:
    return GNSS_SYSTEM_BEIDOU;
  case UBX_GNSS_QZSS:
    return GNSS_SYSTEM_QZSS;
  case UBX_GNSS_GLONASS:
    return GNSS_SYSTEM_GLONASS;
  default:
    return 0;
  }
}

static void m10s_publish_sats(const struct device *dev, const uint8_t *payload, size_t len) {
  struct m10s_data *data = dev->data;
  int n = ubx_nav_sat_count(payload, len);
  uint16_t cnt = 0;

  for (int i = 0; i < n && cnt < ARRAY_SIZE(data->sats); i++) {
    struct ubx_sat s;

    if (ubx_nav_sat_get(payload, len, i, &s) != 0) {
      break;
    }
    data->sats[cnt++] = (struct gnss_satellite){
        .prn = s.sv_id,
        .snr = s.cno_dbhz,
        .elevation = s.elev_deg < 0 ? 0 : (uint8_t)s.elev_deg,
        .azimuth = s.azim_deg < 0 ? 0 : (uint16_t)(s.azim_deg % 360),
        .system = sat_system(s.gnss_id),
        .is_tracked = UBX_SAT_QUALITY(s.flags) >= 4,
        .is_corrected = UBX_SAT_DIFF_CORR(s.flags),
    };
  }
  gnss_publish_satellites(dev, data->sats, cnt);
}
#endif

static void m10s_handle(const struct device *dev, const struct ubx_msg *msg) {
  struct m10s_data *data = dev->data;

  if (msg->cls == UBX_CLASS_NAV && msg->id == UBX_ID_NAV_PVT) {
    struct ubx_nav_pvt pvt;

    if (ubx_parse_nav_pvt(msg->payload, msg->len, &pvt) == 0) {
      m10s_publish_pvt(dev, &pvt);
    }
#if CONFIG_GNSS_SATELLITES
  } else if (msg->cls == UBX_CLASS_NAV && msg->id == UBX_ID_NAV_SAT) {
    m10s_publish_sats(dev, msg->payload, msg->len);
#endif
  } else if (msg->cls == UBX_CLASS_ACK && data->ack_state < 0) {
    int r = ubx_ack_result(msg, data->ack_cls, data->ack_id);

    if (r >= 0) {
      data->ack_state = (int8_t)r;
    }
  }
}

/* Read what the module has and run it through the parser. Call with the lock held. */
static int m10s_drain(const struct device *dev) {
  const struct m10s_config *cfg = dev->config;
  struct m10s_data *data = dev->data;
  uint16_t avail;
  int ret = m10s_avail(dev, &avail);

  /* A bounded loop, so a chatty module cannot hold the work queue. */
  for (int i = 0; ret == 0 && avail > 0 && i < 16; i++) {
    size_t n = MIN(avail, sizeof(data->chunk));

    ret = i2c_burst_read_dt(&cfg->i2c, M10S_REG_STREAM, data->chunk, n);
    if (ret != 0) {
      break;
    }
    for (size_t k = 0; k < n; k++) {
      struct ubx_msg msg;
      int r = ubx_parser_feed(&data->parser, data->chunk[k], &msg);

      if (r == 1) {
        m10s_handle(dev, &msg);
      } else if (r < 0) {
        LOG_DBG("UBX frame dropped (%d)", r);
      }
    }
    ret = m10s_avail(dev, &avail);
  }
  return ret;
}

/* Send a frame and wait for its ACK. Call with the lock held. */
static int m10s_send_acked(const struct device *dev, const uint8_t *frame, size_t len) {
  struct m10s_data *data = dev->data;
  int64_t end = k_uptime_get() + M10S_ACK_TIMEOUT_MS;
  int ret;

  data->ack_cls = frame[2];
  data->ack_id = frame[3];
  data->ack_state = -1;
  ret = m10s_write(dev, frame, len);
  if (ret != 0) {
    return ret;
  }
  while (data->ack_state < 0 && k_uptime_get() < end) {
    k_msleep(20);
    ret = m10s_drain(dev);
    if (ret != 0) {
      return ret;
    }
  }
  if (data->ack_state < 0) {
    return -ETIMEDOUT;
  }
  return data->ack_state == 1 ? 0 : -EINVAL;
}

/* ---- Configuration ---------------------------------------------------------------------- */

static uint8_t nav_mode_to_dyn(enum gnss_navigation_mode mode) {
  switch (mode) {
  case GNSS_NAVIGATION_MODE_ZERO_DYNAMICS:
    return UBX_DYN_STATIONARY;
  case GNSS_NAVIGATION_MODE_LOW_DYNAMICS:
    return UBX_DYN_PEDESTRIAN;
  case GNSS_NAVIGATION_MODE_HIGH_DYNAMICS:
    return 7; /* airborne < 2g */
  default:
    return UBX_DYN_PORTABLE;
  }
}

static int signal_keys(gnss_systems_t systems, struct ubx_valset *v) {
  static const struct {
    uint32_t system;
    uint32_t key;
  } map[] = {
      {GNSS_SYSTEM_GPS, UBX_KEY_SIGNAL_GPS_ENA},    {GNSS_SYSTEM_GALILEO, UBX_KEY_SIGNAL_GAL_ENA},
      {GNSS_SYSTEM_BEIDOU, UBX_KEY_SIGNAL_BDS_ENA}, {GNSS_SYSTEM_GLONASS, UBX_KEY_SIGNAL_GLO_ENA},
      {GNSS_SYSTEM_QZSS, UBX_KEY_SIGNAL_QZSS_ENA},  {GNSS_SYSTEM_SBAS, UBX_KEY_SIGNAL_SBAS_ENA},
  };

  for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
    int ret = ubx_valset_add(v, map[i].key, (systems & map[i].system) ? 1 : 0);

    if (ret != 0) {
      return ret;
    }
  }
  return 0;
}

static int power_keys(const struct m10s_data *data, struct ubx_valset *v) {
  int ret = ubx_valset_add(v, UBX_KEY_PM_OPERATEMODE, data->psm_mode);

  if (ret == 0 && data->psm_mode != UBX_PM_FULL) {
    ret = ubx_valset_add(v, UBX_KEY_PM_POSUPDATEPERIOD, data->psm_period_s);
    if (ret == 0) {
      ret = ubx_valset_add(v, UBX_KEY_PM_ACQPERIOD, data->psm_period_s);
    }
  }
  return ret;
}

static int m10s_valset(const struct device *dev, int (*fill)(const struct m10s_data *, struct ubx_valset *)) {
  struct m10s_data *data = dev->data;
  struct ubx_valset v;
  int ret = ubx_valset_init(&v, data->tx, sizeof(data->tx), M10S_LAYERS);

  if (ret == 0) {
    ret = fill(data, &v);
  }
  if (ret != 0) {
    return ret;
  }
  return m10s_send_acked(dev, data->tx, ubx_valset_finish(&v));
}

static int fill_main(const struct m10s_data *data, struct ubx_valset *v) {
  int ret = 0;
  const bool sats = IS_ENABLED(CONFIG_GNSS_SATELLITES) && !data->polled;

  /* UBX only on I2C; a long timeout so a slow reader does not lose the stream. */
  ret |= ubx_valset_add(v, UBX_KEY_I2COUTPROT_UBX, 1);
  ret |= ubx_valset_add(v, UBX_KEY_I2COUTPROT_NMEA, 0);
  ret |= ubx_valset_add(v, UBX_KEY_I2CINPROT_UBX, 1);
  ret |= ubx_valset_add(v, UBX_KEY_I2CINPROT_NMEA, 0);
  ret |= ubx_valset_add(v, UBX_KEY_I2C_EXTENDEDTIMEOUT, 1);
  ret |= ubx_valset_add(v, UBX_KEY_MSGOUT_NAV_PVT_I2C, data->polled ? 0 : 1);
  ret |= ubx_valset_add(v, UBX_KEY_MSGOUT_NAV_SAT_I2C, sats ? 1 : 0);
  ret |= ubx_valset_add(v, UBX_KEY_RATE_MEAS, data->fix_interval_ms);
  ret |= ubx_valset_add(v, UBX_KEY_RATE_NAV, 1);
  ret |= ubx_valset_add(v, UBX_KEY_NAVSPG_DYNMODEL, nav_mode_to_dyn(data->nav_mode));
  return ret ? -ENOSPC : 0;
}

static int fill_power_signals(const struct m10s_data *data, struct ubx_valset *v) {
  int ret = power_keys(data, v);

  if (ret == 0 && data->systems_set) {
    ret = signal_keys(data->systems, v);
  }
  return ret;
}

static int m10s_configure(const struct device *dev) {
  int ret = m10s_valset(dev, fill_main);

  if (ret == 0) {
    ret = m10s_valset(dev, fill_power_signals);
  }
  return ret;
}

/* ---- Work ------------------------------------------------------------------------------- */

static int m10s_send_polls(const struct device *dev) {
  struct m10s_data *data = dev->data;
  int n = ubx_poll_encode(data->tx, sizeof(data->tx), UBX_CLASS_NAV, UBX_ID_NAV_PVT);
  int ret = m10s_write(dev, data->tx, n);

  if (ret == 0 && IS_ENABLED(CONFIG_GNSS_SATELLITES)) {
    n = ubx_poll_encode(data->tx, sizeof(data->tx), UBX_CLASS_NAV, UBX_ID_NAV_SAT);
    ret = m10s_write(dev, data->tx, n);
  }
  return ret;
}

static int m10s_enter_standby(const struct device *dev) {
  const struct m10s_config *cfg = dev->config;
  struct m10s_data *data = dev->data;
  uint32_t wake = cfg->extint.port ? UBX_PMREQ_WAKE_EXTINT0 : 0;
  int n = ubx_pmreq_encode(data->tx, sizeof(data->tx), 0, UBX_PMREQ_FLAG_BACKUP, wake);

  return m10s_write(dev, data->tx, n);
}

static void m10s_work(struct k_work *work) {
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  struct m10s_data *data = CONTAINER_OF(dwork, struct m10s_data, work);
  const struct device *dev = data->dev;
  const struct m10s_config *cfg = dev->config;
  uint32_t next_ms = data->polled ? data->fix_interval_ms : cfg->poll_interval_ms;
  int ret = 0;

  k_mutex_lock(&data->lock, K_FOREVER);
  if (data->bus_lock != NULL && data->bus_lock(data->bus_ctx) != 0) {
    /* Someone else (the GAP9 camera) owns the bus: no I2C this round. */
    k_work_reschedule(&data->work, K_MSEC(M10S_BUSY_RETRY_MS));
    k_mutex_unlock(&data->lock);
    return;
  }
  if (!data->active) {
    if (cfg->standby_at_boot && data->standby_tries > 0) {
      /* Boot-time standby: the module may need a moment before it answers. */
      uint16_t avail;

      ret = m10s_avail(dev, &avail);
      if (ret == 0) {
        ret = m10s_enter_standby(dev);
      }
      if (ret == 0) {
        LOG_INF("MAX-M10S in standby");
        data->standby_tries = 0;
        goto out;
      }
      data->standby_tries--;
      if (data->standby_tries == 0) {
        LOG_WRN("MAX-M10S did not answer, standby not sent");
        goto out;
      }
      k_work_reschedule(&data->work, K_MSEC(M10S_RETRY_MS));
    }
    goto out;
  }

  if (!data->configured) {
    ret = m10s_configure(dev);
    if (ret != 0) {
      LOG_WRN("MAX-M10S configuration failed (%d), retry", ret);
      k_work_reschedule(&data->work, K_MSEC(M10S_RETRY_MS));
      goto out;
    }
    data->configured = true;
    LOG_INF("MAX-M10S configured");
  }

  if (data->polled) {
    if (data->poll_wait) {
      ret = m10s_drain(dev);
      data->poll_wait = false;
      next_ms = data->fix_interval_ms > 100 ? data->fix_interval_ms - 100 : 1;
    } else {
      ret = m10s_send_polls(dev);
      data->poll_wait = ret == 0;
      next_ms = 100;
    }
  } else {
    ret = m10s_drain(dev);
  }
  if (ret != 0) {
    LOG_DBG("I2C error %d", ret);
    data->poll_wait = false;
    next_ms = M10S_RETRY_MS;
  }
  k_work_reschedule(&data->work, K_MSEC(next_ms));
out:
  if (data->bus_unlock != NULL) {
    data->bus_unlock(data->bus_ctx);
  }
  k_mutex_unlock(&data->lock);
}

/* ---- GNSS API --------------------------------------------------------------------------- */

static int m10s_set_fix_rate(const struct device *dev, uint32_t fix_interval_ms) {
  struct m10s_data *data = dev->data;
  int ret = 0;

  if (fix_interval_ms < 50 || fix_interval_ms > UINT16_MAX) {
    return -EINVAL;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  data->fix_interval_ms = fix_interval_ms;
  if (data->configured) {
    struct ubx_valset v;

    ubx_valset_init(&v, data->tx, sizeof(data->tx), M10S_LAYERS);
    ret = ubx_valset_add(&v, UBX_KEY_RATE_MEAS, fix_interval_ms);
    if (ret == 0) {
      ret = m10s_send_acked(dev, data->tx, ubx_valset_finish(&v));
    }
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

static int m10s_get_fix_rate(const struct device *dev, uint32_t *fix_interval_ms) {
  const struct m10s_data *data = dev->data;

  *fix_interval_ms = data->fix_interval_ms;
  return 0;
}

static int m10s_set_navigation_mode(const struct device *dev, enum gnss_navigation_mode mode) {
  struct m10s_data *data = dev->data;
  int ret = 0;

  if (mode > GNSS_NAVIGATION_MODE_HIGH_DYNAMICS) {
    return -EINVAL;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  data->nav_mode = mode;
  if (data->configured) {
    struct ubx_valset v;

    ubx_valset_init(&v, data->tx, sizeof(data->tx), M10S_LAYERS);
    ret = ubx_valset_add(&v, UBX_KEY_NAVSPG_DYNMODEL, nav_mode_to_dyn(mode));
    if (ret == 0) {
      ret = m10s_send_acked(dev, data->tx, ubx_valset_finish(&v));
    }
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

static int m10s_get_navigation_mode(const struct device *dev, enum gnss_navigation_mode *mode) {
  const struct m10s_data *data = dev->data;

  *mode = data->nav_mode;
  return 0;
}

static int m10s_set_enabled_systems(const struct device *dev, gnss_systems_t systems) {
  struct m10s_data *data = dev->data;
  int ret = 0;

  if ((systems & ~M10S_SYSTEMS_SUPPORTED) || systems == 0) {
    return -EINVAL;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  if (data->configured) {
    struct ubx_valset v;

    ubx_valset_init(&v, data->tx, sizeof(data->tx), M10S_LAYERS);
    ret = signal_keys(systems, &v);
    if (ret == 0) {
      ret = m10s_send_acked(dev, data->tx, ubx_valset_finish(&v));
    }
  }
  if (ret == 0) {
    data->systems = systems;
    data->systems_set = true;
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

static int m10s_get_enabled_systems(const struct device *dev, gnss_systems_t *systems) {
  const struct m10s_data *data = dev->data;

  *systems = data->systems;
  return 0;
}

static int m10s_get_supported_systems(const struct device *dev, gnss_systems_t *systems) {
  ARG_UNUSED(dev);
  *systems = M10S_SYSTEMS_SUPPORTED;
  return 0;
}

static DEVICE_API(gnss, m10s_api) = {
    .set_fix_rate = m10s_set_fix_rate,
    .get_fix_rate = m10s_get_fix_rate,
    .set_navigation_mode = m10s_set_navigation_mode,
    .get_navigation_mode = m10s_get_navigation_mode,
    .set_enabled_systems = m10s_set_enabled_systems,
    .get_enabled_systems = m10s_get_enabled_systems,
    .get_supported_systems = m10s_get_supported_systems,
};

/* ---- SENSEI extras ---------------------------------------------------------------------- */

int gnss_m10s_set_power_save(const struct device *dev, enum gnss_m10s_power_save mode, uint32_t period_s) {
  struct m10s_data *data = dev->data;
  int ret = 0;

  if (mode > GNSS_M10S_PSM_CYCLIC || (mode != GNSS_M10S_PSM_FULL && period_s < 5)) {
    return -EINVAL;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  data->psm_mode = mode;
  data->psm_period_s = period_s;
  if (data->configured) {
    ret = m10s_valset(dev, power_keys);
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

int gnss_m10s_poll(const struct device *dev) {
  struct m10s_data *data = dev->data;
  int ret = -EAGAIN;

  k_mutex_lock(&data->lock, K_FOREVER);
  if (data->active && data->configured) {
    ret = m10s_send_polls(dev);
    if (ret == 0) {
      data->poll_wait = data->polled;
      /* The answer is read by the work item shortly after. */
      k_work_reschedule(&data->work, K_MSEC(100));
    }
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

int gnss_m10s_standby(const struct device *dev) {
  struct m10s_data *data = dev->data;
  struct k_work_sync sync;
  int ret;

  k_work_cancel_delayable_sync(&data->work, &sync);
  k_mutex_lock(&data->lock, K_FOREVER);
  ret = m10s_enter_standby(dev);
  if (ret == 0) {
    data->active = false;
    data->configured = false;
    data->poll_wait = false;
    data->standby_tries = 0;
  }
  k_mutex_unlock(&data->lock);
  return ret;
}

int gnss_m10s_reset(const struct device *dev) {
  const struct m10s_config *cfg = dev->config;
  struct m10s_data *data = dev->data;

  if (cfg->reset.port == NULL) {
    return -ENOTSUP;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  gpio_pin_set_dt(&cfg->reset, 1);
  k_msleep(5); /* the module needs at least 1 ms */
  gpio_pin_set_dt(&cfg->reset, 0);
  data->configured = false;
  data->poll_wait = false;
  if (data->active) {
    k_work_reschedule(&data->work, K_MSEC(M10S_BOOT_MS));
  }
  k_mutex_unlock(&data->lock);
  return 0;
}

int gnss_m10s_wake(const struct device *dev) {
  const struct m10s_config *cfg = dev->config;
  struct m10s_data *data = dev->data;

  if (cfg->extint.port == NULL && cfg->reset.port == NULL) {
    return -ENOTSUP;
  }
  if (cfg->extint.port != NULL) {
    /* One rising edge on EXTINT wakes the module (wake-up source set in the PMREQ). */
    gpio_pin_set_dt(&cfg->extint, 1);
    k_msleep(M10S_WAKE_PULSE_MS);
    gpio_pin_set_dt(&cfg->extint, 0);
    k_mutex_lock(&data->lock, K_FOREVER);
    data->active = true;
    data->configured = false;
    k_work_reschedule(&data->work, K_MSEC(M10S_BOOT_MS));
    k_mutex_unlock(&data->lock);
    return 0;
  }
  k_mutex_lock(&data->lock, K_FOREVER);
  data->active = true;
  k_mutex_unlock(&data->lock);
  return gnss_m10s_reset(dev);
}

void gnss_m10s_set_bus_hooks(const struct device *dev, int (*lock)(void *ctx), void (*unlock)(void *ctx), void *ctx) {
  struct m10s_data *data = dev->data;

  k_mutex_lock(&data->lock, K_FOREVER);
  data->bus_lock = lock;
  data->bus_unlock = unlock;
  data->bus_ctx = ctx;
  k_mutex_unlock(&data->lock);
}

int gnss_m10s_get_pvt_ext(const struct device *dev, struct gnss_m10s_pvt_ext *out) {
  struct m10s_data *data = dev->data;
  k_spinlock_key_t key = k_spin_lock(&data->pvt_lock);

  *out = data->pvt_ext;
  k_spin_unlock(&data->pvt_lock, key);
  return out->valid ? 0 : -EAGAIN;
}

bool gnss_m10s_is_configured(const struct device *dev) {
  const struct m10s_data *data = dev->data;

  return data->configured;
}

static int __maybe_unused m10s_pm_action(const struct device *dev, enum pm_device_action action) {
  switch (action) {
  case PM_DEVICE_ACTION_SUSPEND:
    return gnss_m10s_standby(dev);
  case PM_DEVICE_ACTION_RESUME:
    return gnss_m10s_wake(dev);
  default:
    return -ENOTSUP;
  }
}

/* ---- Init ------------------------------------------------------------------------------- */

static int m10s_init(const struct device *dev) {
  const struct m10s_config *cfg = dev->config;
  struct m10s_data *data = dev->data;
  int ret;

  if (!i2c_is_ready_dt(&cfg->i2c)) {
    return -ENODEV;
  }
  data->dev = dev;
  data->fix_interval_ms = cfg->fix_interval_ms;
  data->polled = cfg->polled;
  data->psm_mode = cfg->psm_mode;
  data->psm_period_s = cfg->psm_period_s;
  data->nav_mode = GNSS_NAVIGATION_MODE_BALANCED_DYNAMICS;
  data->systems = M10S_SYSTEMS_DEFAULT;
  k_mutex_init(&data->lock);
  k_work_init_delayable(&data->work, m10s_work);
  ubx_parser_init(&data->parser, data->rx, sizeof(data->rx));

  if (cfg->reset.port != NULL) {
    if (!gpio_is_ready_dt(&cfg->reset)) {
      return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
      return ret;
    }
  }
  if (cfg->extint.port != NULL) {
    if (!gpio_is_ready_dt(&cfg->extint)) {
      return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&cfg->extint, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
      return ret;
    }
  }
  if (cfg->timepulse.port != NULL) {
    if (!gpio_is_ready_dt(&cfg->timepulse)) {
      return -ENODEV;
    }
    /* Input only: a low level at start-up would put the module into safeboot. */
    ret = gpio_pin_configure_dt(&cfg->timepulse, GPIO_INPUT);
    if (ret != 0) {
      return ret;
    }
  }

  data->active = !cfg->standby_at_boot;
  data->standby_tries = cfg->standby_at_boot ? M10S_STANDBY_TRIES : 0;
  k_work_schedule(&data->work, K_MSEC(M10S_BOOT_MS));
  return 0;
}

#define M10S_INIT(inst)                                                                                                \
  static struct m10s_data m10s_data_##inst;                                                                            \
  static const struct m10s_config m10s_cfg_##inst = {                                                                  \
      .i2c = I2C_DT_SPEC_INST_GET(inst),                                                                               \
      .reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                                                       \
      .extint = GPIO_DT_SPEC_INST_GET_OR(inst, extint_gpios, {0}),                                                     \
      .timepulse = GPIO_DT_SPEC_INST_GET_OR(inst, timepulse_gpios, {0}),                                               \
      .fix_interval_ms = DT_INST_PROP(inst, fix_rate),                                                                 \
      .poll_interval_ms = DT_INST_PROP(inst, poll_interval_ms),                                                        \
      .psm_period_s = DT_INST_PROP(inst, power_save_period),                                                           \
      .psm_mode = DT_INST_ENUM_IDX(inst, power_save),                                                                  \
      .polled = DT_INST_PROP(inst, polled),                                                                            \
      .standby_at_boot = DT_INST_PROP(inst, standby_at_boot),                                                          \
  };                                                                                                                   \
  PM_DEVICE_DT_INST_DEFINE(inst, m10s_pm_action);                                                                      \
  DEVICE_DT_INST_DEFINE(inst, m10s_init, PM_DEVICE_DT_INST_GET(inst), &m10s_data_##inst, &m10s_cfg_##inst,             \
                        POST_KERNEL, CONFIG_GNSS_INIT_PRIORITY, &m10s_api);

DT_INST_FOREACH_STATUS_OKAY(M10S_INIT)
