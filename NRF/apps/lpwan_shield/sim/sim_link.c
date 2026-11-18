/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sim_link.c
 * @brief The link to the nRF5340 on native_sim: the same interface as link.c (SPIS and pins), a socket instead.
 *
 * The SPI frames are those of the board, the slave core (link_slave.c) is the same code. A transfer is a message
 * from the simulated node; BUSY and EVENT go to it as messages (sensei_lpwan/lpwan_sim.h). The connection is the
 * power of the shield: when the node cuts it, the process restarts and has lost its state, as after a power cycle.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "link.h"
#include "sensei_lpwan/lpwan_sim.h"
#include "sim_shield.h"

LOG_MODULE_REGISTER(link, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static struct link_slave slave;
static K_MUTEX_DEFINE(q_lock);  /* the event queue of the slave core */
static K_MUTEX_DEFINE(tx_lock); /* writes to the node, and the pin levels */
static link_slave_request_fn request_fn;
static void *request_user;

static int lfd = -1; /* listening for the node */
static int cfd = -1; /* the node */
static const uint8_t *armed_tx;
static uint8_t *armed_rx;
static bool armed;
static bool busy = true, event;
static struct lpwan_sim_rx rx;

static struct k_thread link_thread;
K_THREAD_STACK_DEFINE(link_stack, CONFIG_LPWAN_SHIELD_LINK_STACK);

static void write_frame(uint8_t type, const uint8_t *p, size_t n) {
  static uint8_t out[LPWAN_SIM_HEADER + LPWAN_XFER_LEN];
  size_t len = lpwan_sim_pack(type, p, n, out);

  if (cfd >= 0 && len > 0 && lpwan_sim_write(cfd, out, len) != 0) {
    LOG_WRN("write to the node failed");
    lpwan_sim_close(cfd); /* the read side sees it and restarts */
    cfd = -1;
  }
}

/* Under tx_lock. */
static void send_pins(void) {
  const uint8_t pins[2] = {busy, event};

  write_frame(LPWAN_SIM_PINS, pins, sizeof(pins));
}

static int op_arm(void *ctx, const uint8_t *tx, uint8_t *rx_buf, size_t len) {
  ARG_UNUSED(ctx);
  if (len != LPWAN_XFER_LEN) {
    return -EINVAL;
  }
  k_mutex_lock(&tx_lock, K_FOREVER);
  armed_tx = tx;
  armed_rx = rx_buf;
  armed = true;
  k_mutex_unlock(&tx_lock);
  return 0;
}

static void op_busy(void *ctx, bool b) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&tx_lock, K_FOREVER);
  if (b != busy) {
    busy = b;
    send_pins();
  }
  k_mutex_unlock(&tx_lock);
}

static void op_event(void *ctx, bool pending) {
  ARG_UNUSED(ctx);
  k_mutex_lock(&tx_lock, K_FOREVER);
  if (pending != event) {
    event = pending;
    send_pins();
  }
  k_mutex_unlock(&tx_lock);
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

/* One transfer of the node: answer it with what is armed, then let the core read the frame and arm the next. */
static void on_xfer(const uint8_t *frame, size_t len) {
  static uint8_t rsp[LPWAN_XFER_LEN];
  bool was_armed;

  k_mutex_lock(&tx_lock, K_FOREVER);
  was_armed = armed && len == LPWAN_XFER_LEN;
  if (was_armed) {
    memcpy(armed_rx, frame, LPWAN_XFER_LEN);
    memcpy(rsp, armed_tx, LPWAN_XFER_LEN);
    armed = false;
    busy = true; /* the end of the transfer: BUSY high before the answer, as the SPIS interrupt does */
    send_pins();
  } else {
    memset(rsp, 0, sizeof(rsp)); /* nothing armed: the node clocked into a slave that was not ready */
  }
  write_frame(LPWAN_SIM_XFER_RSP, rsp, sizeof(rsp));
  k_mutex_unlock(&tx_lock);
  if (was_armed) {
    (void)link_slave_done(&slave, 0);
  } else {
    LOG_WRN("transfer while BUSY is high: dropped");
  }
}

static void power_off(void) {
  lpwan_sim_close(cfd);
  cfd = -1;
  sim_shield_restart("the node cut the power");
}

static void link_main(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    uint8_t type;
    const uint8_t *p;
    size_t len;
    int n, r;

    if (cfd < 0) {
      int fd = lfd >= 0 ? lpwan_sim_accept(lfd) : -2;

      if (fd >= 0) { /* powered: one node at a time */
        cfd = fd;
        lpwan_sim_close(lfd);
        lfd = -1;
        rx.n = 0;
        LOG_INF("powered by the node: SPI slave ready");
        k_mutex_lock(&tx_lock, K_FOREVER);
        busy = true;
        event = false;
        send_pins();
        k_mutex_unlock(&tx_lock);
        while (link_slave_start(&slave) != 0) {
          k_msleep(10);
        }
        continue;
      }
      k_msleep(1);
      continue;
    }
    n = lpwan_sim_read(cfd, rx.buf + rx.n, lpwan_sim_rx_room(&rx));
    if (n == -2) {
      k_msleep(1);
      continue;
    }
    if (n <= 0) {
      power_off();
      continue; /* not reached */
    }
    rx.n += (size_t)n;
    while ((r = lpwan_sim_rx_next(&rx, &type, &p, &len)) == 1) {
      if (type == LPWAN_SIM_XFER) {
        on_xfer(p, len);
      }
      lpwan_sim_rx_pop(&rx);
    }
    if (r < 0) {
      LOG_ERR("broken stream from the node");
      power_off();
    }
  }
}

int link_start(link_slave_request_fn on_request, void *user) {
  int port = sim_opt_link_port();

  request_fn = on_request;
  request_user = user;
  if (port <= 0) {
    LOG_ERR("no link port (-link_port): the node cannot reach the shield");
    return -ENODEV;
  }
  lfd = lpwan_sim_listen("127.0.0.1", port); /* the node runs next to the shield; the link is trusted */
  if (lfd < 0) {
    LOG_ERR("cannot listen on port %d", port);
    return -EADDRINUSE;
  }
  link_slave_init(&slave, &ops, NULL, request_fn, request_user);
  k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_LINK_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&link_thread, "link");
  LOG_INF("waiting for the node on 127.0.0.1:%d", port);
  return 0;
}

int link_respond(const sensei_LpwanResponse *rsp) { return link_slave_respond(&slave, rsp); }

int link_push_event(const sensei_LpwanEvent *evt) { return link_slave_push_event(&slave, evt); }

void link_feed_watchdog(void) {}

bool link_master_present(void) { return cfd >= 0; }

uint32_t link_dropped_events(void) { return link_slave_dropped_events(&slave); }
