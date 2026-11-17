/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 * - Serge Wüest <swueest@ee.ethz.ch>
 *
 * Adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

/**
 * @file link.c
 * @brief SPI slave transport (see link_slave.h for the protocol core).
 *
 * Why the commissioning version could stop answering after the modem started, and what is
 * different here (details in README.md):
 *  - BUSY was cleared after "arm + k_sleep(10 us)" hacks. Sleeps of a few us round up to a kernel
 *    tick, so the timing depended on what else ran (a debugger or RTT changed it). Here BUSY goes
 *    low only after spi_transceive_cb() returned, and goes high in the SPIS completion callback
 *    (ISR), not in a thread.
 *  - A 24 s semaphore timeout left a transfer armed and the loop armed another one at once. The
 *    driver refuses (the transfer is still pending), and the loop spun on that error at the
 *    highest priority, starving the modem threads. Here a transfer is never given up: it stays
 *    armed until the master comes. A failed arm is retried with a back-off.
 *  - One mutex was shared between the SPI thread and the LTE/LoRa workers, and the SPI thread
 *    held it while it ran the command handler. Here the handler only queues work.
 *  - nrf_modem_lib_init() (seconds) ran in the connect worker while BUSY stayed high. Here the
 *    link answers at any time; the modem boots in its own thread and reports by events.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "link.h"

LOG_MODULE_REGISTER(link, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

#define LINK_NODE DT_ALIAS(lpwan_link)
#define LINK_WDT_MS 15000
#define SPIS_NODE DT_NODELABEL(spi0)

static const struct device *const spis = DEVICE_DT_GET(SPIS_NODE);
static const struct gpio_dt_spec pin_event = GPIO_DT_SPEC_GET(LINK_NODE, event_gpios);
static const struct gpio_dt_spec pin_busy = GPIO_DT_SPEC_GET(LINK_NODE, busy_gpios);
static const struct gpio_dt_spec pin_ack = GPIO_DT_SPEC_GET(LINK_NODE, ack_gpios);

static const struct spi_config spis_cfg = {
    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_SLAVE,
};

static struct link_slave slave;
static int wdt_id = -1;
static K_MUTEX_DEFINE(q_lock);
static K_SEM_DEFINE(done_sem, 0, 1);
static volatile int done_result;

K_THREAD_STACK_DEFINE(link_stack, CONFIG_LPWAN_SHIELD_LINK_STACK);
static struct k_thread link_thread;

/* Runs in the SPIS interrupt: the master must see BUSY high before the thread has looked at data.
 */
static void spis_done(const struct device *dev, int result, void *data) {
  ARG_UNUSED(dev);
  ARG_UNUSED(data);
  gpio_pin_set_dt(&pin_busy, 1);
  done_result = result < 0 ? result : 0;
  k_sem_give(&done_sem);
}

static int op_arm(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
  const struct spi_buf txb = {.buf = (void *)tx, .len = len};
  const struct spi_buf rxb = {.buf = rx, .len = len};
  const struct spi_buf_set txs = {.buffers = &txb, .count = 1};
  const struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};

  ARG_UNUSED(ctx);
  k_sem_reset(&done_sem);
  return spi_transceive_cb(spis, &spis_cfg, &txs, &rxs, spis_done, NULL);
}

static void op_busy(void *ctx, bool busy) {
  ARG_UNUSED(ctx);
  gpio_pin_set_dt(&pin_busy, busy);
}

static void op_event(void *ctx, bool pending) {
  ARG_UNUSED(ctx);
  gpio_pin_set_dt(&pin_event, pending);
}

static void op_lock(void *ctx) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&q_lock, K_FOREVER);
}

static void op_unlock(void *ctx) {
  ARG_UNUSED(ctx);
  k_mutex_unlock(&q_lock);
}

static const struct link_slave_ops ops = {
    .arm = op_arm,
    .set_busy = op_busy,
    .set_event = op_event,
    .lock = op_lock,
    .unlock = op_unlock,
};

static void link_main(void *a, void *b, void *c) {
  int err;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  /* Task watchdog: this thread must come round every few seconds. It waits for the master with a timeout
   * (the armed transfer stays armed), and does not feed while a re-arm keeps failing, so a link that
   * cannot be armed resets the chip. The hardware watchdog behind it catches a frozen kernel. */
  wdt_id = task_wdt_add(LINK_WDT_MS, NULL, NULL);

  /* The first arm may fail only if the driver is not ready: wait and retry. */
  while ((err = link_slave_start(&slave)) != 0) {
    LOG_ERR("arm failed: %d", err);
    k_msleep(100);
    (void)task_wdt_feed(wdt_id);
  }
  LOG_INF("SPI slave ready");

  for (;;) {
    /* An armed transfer stays armed until the master comes: a timeout here only feeds the watchdog. */
    (void)task_wdt_feed(wdt_id);
    if (k_sem_take(&done_sem, K_MSEC(LINK_WDT_MS / 3)) != 0) {
      continue;
    }
    err = link_slave_done(&slave, done_result);
    while (err != 0) {
      /* Could not arm again: BUSY stays high, the master waits. Back off, then retry. */
      LOG_ERR("re-arm failed: %d", err);
      k_msleep(10);
      err = link_slave_done(&slave, -EIO);
    }
  }
}

int link_start(link_slave_request_fn on_request, void *user) {
  int err;

  if (!device_is_ready(spis) || !gpio_is_ready_dt(&pin_event) || !gpio_is_ready_dt(&pin_busy) ||
      !gpio_is_ready_dt(&pin_ack)) {
    return -ENODEV;
  }
  /* BUSY high (not ready) and EVENT low until the first transfer is armed. */
  err = gpio_pin_configure_dt(&pin_busy, GPIO_OUTPUT_ACTIVE);
  err |= gpio_pin_configure_dt(&pin_event, GPIO_OUTPUT_INACTIVE);
  err |= gpio_pin_configure_dt(&pin_ack, GPIO_INPUT);
  if (err) {
    return -EIO;
  }
  link_slave_init(&slave, &ops, NULL, on_request, user);
  k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_LINK_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&link_thread, "link");
  return 0;
}

int link_respond(const sensei_LpwanResponse *rsp) { return link_slave_respond(&slave, rsp); }

int link_push_event(const sensei_LpwanEvent *evt) { return link_slave_push_event(&slave, evt); }

void link_feed_watchdog(void) {
  if (wdt_id >= 0) {
    (void)task_wdt_feed(wdt_id);
  }
}

bool link_master_present(void) { return gpio_pin_get_dt(&pin_ack) > 0; }

uint32_t link_dropped_events(void) { return link_slave_dropped_events(&slave); }
