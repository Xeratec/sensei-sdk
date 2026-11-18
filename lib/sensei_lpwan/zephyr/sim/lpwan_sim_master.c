/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * The shield link of the simulated node: a TCP connection to the shield simulator instead of SPI, BUSY, EVENT and the
 * power switch (protocol: sensei_lpwan/lpwan_sim.h). One thread reads the socket: pin levels update the cached BUSY
 * and EVENT, a transfer answer wakes the client thread that waits in lpwan_sim_master_xfer().
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cmdline.h"
#include "lpwan_sim_master.h"
#include "posix_native_task.h"
#include "sensei_lpwan/lpwan_link.h"
#include "sensei_lpwan/lpwan_sim.h"

LOG_MODULE_REGISTER(sensei_lpwan_sim, CONFIG_SENSEI_LPWAN_LOG_LEVEL);

#define CONNECT_RETRY_MS 20
#define XFER_TIMEOUT_MS 5000

static char *target; /* "host:port", option -lpwan_shield */
static char host[96];
static int port;

static K_MUTEX_DEFINE(fd_lock); /* the socket, and the pending transfer */
static int fd = -1;
static volatile bool want;
static volatile bool busy = true;
static volatile bool event;
static void (*event_cb)(void);

static K_SEM_DEFINE(rsp_sem, 0, 1);
static uint8_t *rsp_buf; /* where the answer of the pending transfer goes, NULL: no transfer waits */
static size_t rsp_cap;
static int rsp_err;

static struct lpwan_sim_rx rx;
static struct k_thread thread;
K_THREAD_STACK_DEFINE(stack, 2048);

static void add_options(void) {
  static struct args_struct_t opts[] = {
      {
          .option = "lpwan_shield",
          .name = "host:port",
          .type = 's',
          .dest = (void *)&target,
          .descript = "Where the simulated LPWAN shield listens (NRF/apps/lpwan_shield on native_sim)",
      },
      ARG_TABLE_ENDMARKER,
  };

  native_add_command_line_opts(opts);
}
NATIVE_TASK(add_options, PRE_BOOT_1, 10);

static void pins_reset(void) {
  busy = true;
  event = false;
}

/* The connection is gone (the shield was cut off, or it restarted): what waits for it fails. Under fd_lock. */
static void drop_locked(void) {
  lpwan_sim_close(fd);
  fd = -1;
  rx.n = 0;
  pins_reset();
  if (rsp_buf != NULL) {
    rsp_err = -ENOTCONN;
    k_sem_give(&rsp_sem);
  }
}

static void handle(uint8_t type, const uint8_t *p, size_t len) {
  if (type == LPWAN_SIM_PINS && len >= 2) {
    bool ev = p[1] != 0;
    bool rise = ev && !event;

    busy = p[0] != 0;
    event = ev;
    if (rise && event_cb != NULL) {
      event_cb();
    }
  } else if (type == LPWAN_SIM_XFER_RSP && rsp_buf != NULL) {
    memcpy(rsp_buf, p, MIN(len, rsp_cap));
    if (len < rsp_cap) {
      memset(rsp_buf + len, 0, rsp_cap - len);
    }
    rsp_err = 0;
    k_sem_give(&rsp_sem);
  }
}

/* Read what the shield sent. Under fd_lock. */
static void pump_locked(void) {
  for (;;) {
    int n = lpwan_sim_read(fd, rx.buf + rx.n, lpwan_sim_rx_room(&rx));
    uint8_t type;
    const uint8_t *p;
    size_t len;
    int r;

    if (n == -2) {
      return;
    }
    if (n <= 0) {
      LOG_INF("shield link closed");
      drop_locked();
      return;
    }
    rx.n += (size_t)n;
    while ((r = lpwan_sim_rx_next(&rx, &type, &p, &len)) == 1) {
      handle(type, p, len);
      lpwan_sim_rx_pop(&rx);
    }
    if (r < 0) {
      LOG_ERR("shield link: broken stream");
      drop_locked();
      return;
    }
  }
}

static void link_main(void *a, void *b, void *c) {
  int64_t next_try = 0;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    k_mutex_lock(&fd_lock, K_FOREVER);
    if (want && fd < 0 && k_uptime_get() >= next_try) {
      fd = lpwan_sim_connect(host, port);
      next_try = k_uptime_get() + CONNECT_RETRY_MS;
      if (fd >= 0) {
        LOG_INF("shield link up (%s:%d)", host, port);
        rx.n = 0;
      }
    }
    if (fd >= 0) {
      pump_locked();
    }
    k_mutex_unlock(&fd_lock);
    k_msleep(1);
  }
}

static void parse_target(void) {
  const char *tgt = target != NULL ? target : CONFIG_SENSEI_LPWAN_SIM_TARGET; /* the option wins */
  const char *colon = tgt[0] != '\0' ? strrchr(tgt, ':') : NULL;

  if (colon != NULL && colon != tgt) {
    snprintf(host, sizeof(host), "%.*s", (int)MIN((size_t)(colon - tgt), sizeof(host) - 1), tgt);
    port = atoi(colon + 1);
  }
}

bool lpwan_sim_master_configured(void) {
  parse_target();
  return port > 0;
}

int lpwan_sim_master_init(void (*on_event)(void)) {
  event_cb = on_event;
  parse_target();
  if (port <= 0) {
    LOG_WRN("no shield: start with -lpwan_shield=<host>:<port>");
  }
  pins_reset();
  k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), link_main, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0,
                  K_NO_WAIT);
  k_thread_name_set(&thread, "lpwan_sim");
  return 0;
}

int lpwan_sim_master_power(bool on) {
  if (on && port <= 0) {
    return -ENODEV;
  }
  want = on;
  if (!on) {
    k_mutex_lock(&fd_lock, K_FOREVER);
    if (fd >= 0) {
      drop_locked(); /* closing the connection cuts the power: the shield restarts */
    }
    pins_reset();
    k_mutex_unlock(&fd_lock);
  }
  return 0;
}

bool lpwan_sim_master_busy(void) { return busy; }

bool lpwan_sim_master_event(void) { return event; }

int lpwan_sim_master_xfer(const uint8_t *tx, uint8_t *rx_out, size_t len) {
  static uint8_t out[LPWAN_SIM_HEADER + LPWAN_XFER_LEN];
  size_t n = lpwan_sim_pack(LPWAN_SIM_XFER, tx, len, out);
  int err;

  k_mutex_lock(&fd_lock, K_FOREVER);
  if (fd < 0 || n == 0) {
    k_mutex_unlock(&fd_lock);
    return -ENOTCONN;
  }
  k_sem_reset(&rsp_sem);
  rsp_buf = rx_out;
  rsp_cap = len;
  rsp_err = -ETIMEDOUT;
  if (lpwan_sim_write(fd, out, n) != 0) {
    drop_locked();
    rsp_buf = NULL;
    k_mutex_unlock(&fd_lock);
    return -ENOTCONN;
  }
  k_mutex_unlock(&fd_lock);
  (void)k_sem_take(&rsp_sem, K_MSEC(XFER_TIMEOUT_MS));
  k_mutex_lock(&fd_lock, K_FOREVER);
  rsp_buf = NULL;
  err = rsp_err;
  k_mutex_unlock(&fd_lock);
  return err;
}
