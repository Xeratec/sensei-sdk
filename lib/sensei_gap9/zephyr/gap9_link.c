/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* GAP9 link, platform part: threads, GPIOs, SPIS3, power. The protocol is in src/gap9_core.c. */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "pwr/pwr.h"

#include "gap9_priv.h"
#include "sensei_busarb/bus_arb.h"
#include "sensei_gap9/gap9_zephyr.h"

LOG_MODULE_REGISTER(gap9, CONFIG_SENSEI_GAP9_LOG_LEVEL);

#define SPIS_NODE DT_ALIAS(gap9_spis)

static const struct gpio_dt_spec trig_pin = GPIO_DT_SPEC_GET(DT_ALIAS(gap9_trigger), gpios);
static const struct gpio_dt_spec spi_sel = GPIO_DT_SPEC_GET(DT_ALIAS(gap9_spi_sel), gpios);
static const struct gpio_dt_spec i2c_sel = GPIO_DT_SPEC_GET(DT_ALIAS(gap9_i2c_sel), gpios);
#ifdef CONFIG_SENSEI_GAP9_SHARED_GPIOS
static const struct gpio_dt_spec shared_sel = GPIO_DT_SPEC_GET(DT_ALIAS(gap9_shared_sel), gpios);
#endif
static const struct device *const spis = DEVICE_DT_GET(SPIS_NODE);
PINCTRL_DT_DEFINE(SPIS_NODE); /* own copy of the pin states: the driver's is static */

/* One thread with its own work queue runs the link (not the app work queue: the bus lock is held
 * across work items, and a sensor job on the app work queue may wait for it). */
static K_THREAD_STACK_DEFINE(link_stack, CONFIG_SENSEI_GAP9_THREAD_STACK_SIZE);
static struct k_work_q link_wq;
static struct k_work_delayable timer_work;
static struct k_work spi_work;
static struct k_work_delayable submit_work;
static struct k_work shutdown_work;
static struct k_work reset_work;

static const struct gap9_link_cfg *lcfg;
static const struct gap9_link_hooks *hooks;
static struct g9 core;
static uint8_t rxbuf[G9L_RX_CAP] __aligned(4);
static const struct spi_config spi_cfg = {
    .operation = SPI_OP_MODE_SLAVE | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};
static volatile bool spis_armed;
static volatile int spis_rx_amount;
static bool trig_want;
static volatile bool link_ready; /* gap9_link_init() is done */

static void report_error(void) {
  if (hooks != NULL && hooks->error != NULL) {
    hooks->error();
  }
}

/* ---- shared SPI_A and LTE peaks (sensei_busarb) ---- */

#define BUS_HOLD_MS 60000 /* renewed on every link event, so only a dead link thread lets it lapse */
#define BUS_RETRY_MS 250

static bool bus_held;          /* link thread only */
static int64_t bus_wait_since; /* first refused attempt of the current wait, 0 = not waiting */

/* GAP9 may power up only when the LPWAN client holds neither SPI_A nor an LTE transmission. */
static bool bus_take(void) {
  if (bus_claim(BUS_GAP9, BUS_HOLD_MS)) {
    bus_held = true;
    return true;
  }
  return false;
}

/* Give the bus back once GAP9 is off and idle; keep the claim alive while it is on. */
static void bus_sync(void) {
  if (!bus_held) {
    return;
  }
  if (g9_busy(&core)) {
    (void)bus_claim(BUS_GAP9, BUS_HOLD_MS);
  } else {
    bus_release(BUS_GAP9);
    bus_held = false;
  }
}

/* ---- SPIS3: the only place that enables or parks it ---- */

/*
 * Who owns SPI_A (P0.08 ... P0.11): GAP9 (SPIS3 pins active, mux P0.20 high) or nobody (pins
 * parked, mux low). The LPWAN client's SPIM4 uses the same pins; sensei_busarb makes sure only one
 * of them owns the bus (see bus_take() below).
 */
static void gap9_spi_bus(bool gap9) {
  if (gap9) {
    gpio_pin_set_dt(&spi_sel, 1);
    pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPIS_NODE), PINCTRL_STATE_DEFAULT);
  } else {
    pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPIS_NODE), PINCTRL_STATE_SLEEP);
    gpio_pin_set_dt(&spi_sel, 0);
  }
}

static void camera_clock(bool on) {
  if (hooks != NULL && hooks->camera_clock != NULL) {
    hooks->camera_clock(on);
  }
}

static void spis_cb(const struct device *dev, int result, void *data) {
  ARG_UNUSED(dev);
  ARG_UNUSED(data);
  /* The master is done. Tell it to wait until the link thread has re-armed. */
  gpio_pin_set_dt(&trig_pin, 0);
  spis_armed = false;
  spis_rx_amount = result > 0 ? result : 0;
  k_work_submit_to_queue(&link_wq, &spi_work);
}

/* Issue the transaction once: TX is the frame buffer of the core, RX as long. */
static void spis_ensure_armed(void) {
  if (spis_armed) {
    return;
  }
  const struct spi_buf tx = {.buf = core.tx, .len = sizeof(core.tx)};
  const struct spi_buf rx = {.buf = rxbuf, .len = sizeof(rxbuf)};
  const struct spi_buf_set txs = {.buffers = &tx, .count = 1};
  const struct spi_buf_set rxs = {.buffers = &rx, .count = 1};

  spis_armed = true;
  int ret = spi_transceive_cb(spis, &spi_cfg, &txs, &rxs, spis_cb, NULL);

  if (ret != 0) {
    spis_armed = false;
    LOG_ERR("SPIS arm failed (%d)", ret);
    report_error();
  }
}

/* After every handler: SPIS armed again, then the trigger line may go high. */
static void link_sync(void) {
  bus_sync();
  if (core.state == G9_OFF || core.state == G9_CYCLE) {
    return;
  }
  spis_ensure_armed();
  gpio_pin_set_dt(&trig_pin, trig_want && spis_armed);
}

/* ---- core operations ---- */

static void op_power(bool on) {
  if (on) {
    camera_clock(true);
    gpio_pin_set_dt(&i2c_sel, 1);
#ifdef CONFIG_SENSEI_GAP9_SHARED_GPIOS
    gpio_pin_set_dt(&shared_sel, 1);
#endif
    gap9_spi_bus(true);
    spis_ensure_armed();
    gap9_pwr(true);
  } else {
    gap9_pwr(false);
    gap9_spi_bus(false);
    gpio_pin_set_dt(&i2c_sel, 0);
    camera_clock(false);
#ifdef CONFIG_SENSEI_GAP9_SHARED_GPIOS
    gpio_pin_set_dt(&shared_sel, 0);
#endif
    gpio_pin_set_dt(&trig_pin, 0);
    trig_want = false;
  }
  LOG_INF("GAP9 %s", on ? "on" : "off");
}

static void op_trigger(bool high) {
  trig_want = high;
  if (!high) {
    gpio_pin_set_dt(&trig_pin, 0);
  }
  /* high is applied in link_sync() once the SPIS is armed */
}

static void op_bus_lock(void) {
  if (hooks != NULL && hooks->bus_lock != NULL) {
    hooks->bus_lock();
  }
}

static void op_bus_unlock(void) {
  if (hooks != NULL && hooks->bus_unlock != NULL) {
    hooks->bus_unlock();
  }
}

static void op_timer(uint32_t ms) {
  if (ms == 0) {
    k_work_cancel_delayable(&timer_work);
  } else {
    k_work_reschedule_for_queue(&link_wq, &timer_work, K_MSEC(ms));
  }
}

/* Results go to the delivery work queue through a ring. The link thread waits when it is full. */
struct delivery {
  g9_done_fn fn;
  void *user;
  int err;
  bool has_res;
  bool final;
};
static struct delivery ring[CONFIG_SENSEI_GAP9_RESULT_RING];
static unsigned int ring_head, ring_tail;
static K_SEM_DEFINE(ring_free, CONFIG_SENSEI_GAP9_RESULT_RING, CONFIG_SENSEI_GAP9_RESULT_RING);
static struct k_work deliver_work;

static void *ring_res(unsigned int i) {
  return (uint8_t *)lcfg->storage.ring + (size_t)(i % CONFIG_SENSEI_GAP9_RESULT_RING) * lcfg->codec->result_size;
}

static void deliver_handler(struct k_work *w) {
  ARG_UNUSED(w);
  while (ring_tail != ring_head) {
    struct delivery *d = &ring[ring_tail % CONFIG_SENSEI_GAP9_RESULT_RING];

    d->fn(d->err, d->has_res ? ring_res(ring_tail) : NULL, d->final, d->user);
    ring_tail++;
    k_sem_give(&ring_free);
  }
}

/*
 * A frame that is not final (a JPEG chunk) is dropped when the ring stays full for 2 s. A final
 * frame ends the job, so it waits for room as long as it takes: the callback frees the job slot
 * of the app, and the link would never finish the job otherwise.
 */
static void op_deliver(g9_done_fn fn, void *user, int err, const void *res, bool final) {
  if (fn == NULL) {
    return;
  }
  if (k_sem_take(&ring_free, final ? K_FOREVER : K_SECONDS(2)) != 0) {
    LOG_ERR("result ring full, frame dropped");
    report_error();
    return;
  }
  struct delivery *d = &ring[ring_head % CONFIG_SENSEI_GAP9_RESULT_RING];

  d->fn = fn;
  d->user = user;
  d->err = err;
  d->final = final;
  d->has_res = res != NULL;
  if (res != NULL) {
    memcpy(ring_res(ring_head), res, lcfg->codec->result_size);
  }
  ring_head++;
  if (hooks != NULL && hooks->deliver_wq != NULL) {
    k_work_submit_to_queue(hooks->deliver_wq, &deliver_work);
  } else {
    k_work_submit(&deliver_work);
  }
}

static void op_error(void) { report_error(); }

static void op_marker(bool on) {
  if (hooks != NULL && hooks->marker != NULL) {
    hooks->marker(on);
  }
}

static const struct g9_ops ops = {
    .power = op_power,
    .trigger = op_trigger,
    .bus_lock = op_bus_lock,
    .bus_unlock = op_bus_unlock,
    .timer = op_timer,
    .deliver = op_deliver,
    .error = op_error,
    .marker = op_marker,
};

/* ---- work handlers (all run on the link thread) ---- */

static void spi_handler(struct k_work *w) {
  ARG_UNUSED(w);
  int n = spis_rx_amount;

  if (n > 0) {
    g9_spi_done(&core, rxbuf, (size_t)n);
  }
  link_sync();
}

static void timer_handler(struct k_work *w) {
  ARG_UNUSED(w);
  g9_timer_expired(&core);
  link_sync();
}

/* Submissions are staged by the caller's thread and copied into the core here. */
struct stage {
  g9_done_fn done;
  void *user;
};
/* The link thread (higher priority) moves a staged job into the core queue at once. It stays
 * when the job waits for SPI_A or LTE (bus_take()) or for a free queue entry. */
#define STAGE_DEPTH CONFIG_SENSEI_GAP9_STAGE_DEPTH
static struct stage stages[STAGE_DEPTH];
static unsigned int stage_head, stage_tail;
static struct k_spinlock stage_lock;

static void *stage_job(unsigned int i) {
  return (uint8_t *)lcfg->storage.stage + (size_t)(i % STAGE_DEPTH) * lcfg->codec->job_size;
}

static void submit_handler(struct k_work *w) {
  ARG_UNUSED(w);
  for (;;) {
    struct stage *s = NULL;
    k_spinlock_key_t k = k_spin_lock(&stage_lock);

    if (stage_tail != stage_head) {
      s = &stages[stage_tail % STAGE_DEPTH];
    }
    k_spin_unlock(&stage_lock, k);
    if (s == NULL) {
      break;
    }
    int ret = 0;

    if (!bus_held && !bus_take()) {
      /* LPWAN holds SPI_A or an LTE transmission runs: wait, then give up. */
      int64_t now = k_uptime_get();

      if (bus_wait_since == 0) {
        bus_wait_since = now;
      }
      if (now - bus_wait_since < CONFIG_SENSEI_GAP9_BUS_WAIT_S * 1000LL) {
        k_work_reschedule_for_queue(&link_wq, &submit_work, K_MSEC(BUS_RETRY_MS));
        return;
      }
      LOG_WRN("SPI_A or LTE busy for %d s, job dropped", CONFIG_SENSEI_GAP9_BUS_WAIT_S);
      ret = -EBUSY;
    } else if (g9_full(&core)) {
      /* All queue entries are in use: a result will free one (jobs have timeouts). */
      k_work_reschedule_for_queue(&link_wq, &submit_work, K_MSEC(BUS_RETRY_MS));
      return;
    } else {
      bus_wait_since = 0;
      ret = g9_submit(&core, stage_job(stage_tail), s->done, s->user);
    }
    if (ret != 0) {
      report_error();
      op_deliver(s->done, s->user, ret, NULL, true);
    }
    k = k_spin_lock(&stage_lock);
    stage_tail++;
    k_spin_unlock(&stage_lock, k);
  }
  bus_wait_since = 0;
  link_sync();
}

static void shutdown_handler(struct k_work *w) {
  ARG_UNUSED(w);
  g9_shutdown(&core);
  /* Jobs that are still staged (waiting for SPI_A, LTE or a queue entry) fail too. */
  k_work_cancel_delayable(&submit_work);
  while (stage_tail != stage_head) {
    struct stage *s = &stages[stage_tail % STAGE_DEPTH];

    op_deliver(s->done, s->user, -ECANCELED, NULL, true);
    k_spinlock_key_t k = k_spin_lock(&stage_lock);

    stage_tail++;
    k_spin_unlock(&stage_lock, k);
  }
  bus_wait_since = 0;
  link_sync();
}

static void reset_handler(struct k_work *w) {
  ARG_UNUSED(w);
  if (core.state != G9_OFF) {
    LOG_WRN("GAP9 reset by button");
  }
  g9_reset(&core);
  link_sync();
}

/* ---- public API (gap9_zephyr.h) ---- */

int gap9_link_init(const struct gap9_link_cfg *cfg) {
  const struct gpio_dt_spec *outs[] = {
      &trig_pin,
      &spi_sel,
      &i2c_sel,
#ifdef CONFIG_SENSEI_GAP9_SHARED_GPIOS
      &shared_sel,
#endif
  };

  lcfg = cfg;
  hooks = cfg->hooks;
  for (size_t i = 0; i < ARRAY_SIZE(outs); i++) {
    if (!gpio_is_ready_dt(outs[i]) || gpio_pin_configure_dt(outs[i], GPIO_OUTPUT_INACTIVE)) {
      return -ENODEV;
    }
  }
  if (!device_is_ready(spis)) {
    return -ENODEV;
  }
  gap9_spi_bus(false);
  camera_clock(false);

  const struct g9_cfg gcfg = {
      .boot_ms = CONFIG_SENSEI_GAP9_BOOT_TIMEOUT_MS,
      .busfree_ms = CONFIG_SENSEI_GAP9_BUSFREE_TIMEOUT_MS,
      .job_ms = CONFIG_SENSEI_GAP9_JOB_TIMEOUT_MS,
      .idle_off_ms = CONFIG_SENSEI_GAP9_IDLE_OFF_MS,
      .restart_off_ms = CONFIG_SENSEI_GAP9_RESTART_OFF_MS,
  };

  g9_init(&core, &ops, cfg->codec, &gcfg, cfg->storage.jobs, cfg->storage.result);
  k_work_queue_start(&link_wq, link_stack, K_THREAD_STACK_SIZEOF(link_stack), CONFIG_SENSEI_GAP9_THREAD_PRIORITY, NULL);
  k_thread_name_set(&link_wq.thread, "gap9");
  k_work_init(&spi_work, spi_handler);
  k_work_init_delayable(&submit_work, submit_handler);
  k_work_init(&shutdown_work, shutdown_handler);
  k_work_init(&reset_work, reset_handler);
  k_work_init(&deliver_work, deliver_handler);
  k_work_init_delayable(&timer_work, timer_handler);
  link_ready = true;
  return 0;
}

int gap9_link_submit(const void *job, g9_done_fn done, void *user) {
  k_spinlock_key_t k = k_spin_lock(&stage_lock);

  if (stage_head - stage_tail >= STAGE_DEPTH) {
    k_spin_unlock(&stage_lock, k);
    return -ENOMEM;
  }
  struct stage *s = &stages[stage_head % STAGE_DEPTH];

  memcpy(stage_job(stage_head), job, lcfg->codec->job_size); /* a few microseconds */
  s->done = done;
  s->user = user;
  stage_head++;
  k_spin_unlock(&stage_lock, k);
  k_work_reschedule_for_queue(&link_wq, &submit_work, K_NO_WAIT);
  return 0;
}

bool gap9_link_busy(void) { return g9_busy(&core) || stage_head != stage_tail; }

void gap9_link_power_off_now(void) {
  if (link_ready) {
    k_work_submit_to_queue(&link_wq, &shutdown_work);
  }
}

/* Called by the power thread (button): may come before the link is up. */
void gap9_link_reset(void) {
  if (link_ready) {
    k_work_submit_to_queue(&link_wq, &reset_work);
  }
}

void gap9_link_status_get(struct gap9_link_status *st) {
  st->state = core.state;
  st->errors = core.errors;
  st->version = core.version;
  st->busy = gap9_link_busy();
}

int gap9_link_ping(uint32_t timeout_ms, uint32_t *duration_us, sensei_Version *ver) {
  if (hooks == NULL || hooks->ping == NULL) {
    return -ENOTSUP;
  }
  return hooks->ping(timeout_ms, duration_us, ver);
}
