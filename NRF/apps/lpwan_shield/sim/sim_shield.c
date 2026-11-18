/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Command line, control connection and restart of the simulated shield. */

#define _POSIX_C_SOURCE 200809L /* strtok_r */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "cmdline.h"
#include "posix_native_task.h"
#include "sensei_lpwan/lpwan_sim.h"
#include "sim_shield.h"

LOG_MODULE_REGISTER(sim_shield, CONFIG_LPWAN_SHIELD_LOG_LEVEL);

static int link_port;
static int ctl_port;
static char *bind_addr;
static char *lr_state;
static char *gnss_opt;

static void add_options(void) {
  static struct args_struct_t opts[] = {
      {
          .option = "link_port",
          .name = "port",
          .type = 'i',
          .dest = (void *)&link_port,
          .descript = "TCP port on which the node simulator connects (the SPI link)",
      },
      {
          .option = "ctl_port",
          .name = "port",
          .type = 'i',
          .dest = (void *)&ctl_port,
          .descript = "TCP port of the control connection to the test harness (network server, commands)",
      },
      {
          .option = "bind",
          .name = "address",
          .type = 's',
          .dest = (void *)&bind_addr,
          .descript = "Address to listen on, default 127.0.0.1",
      },
      {
          .option = "lr_state",
          .name = "file",
          .type = 's',
          .dest = (void *)&lr_state,
          .descript = "File with what the LR1121 keeps in its flash (LoRaWAN session), default: none",
      },
      {
          .option = "gnss",
          .name = "scenario",
          .type = 's',
          .dest = (void *)&gnss_opt,
          .descript = "Scenario of the fake GNSS, the words of the control command 'gnss', e.g. \"fix 5 473764000 "
                      "85476000 410 5 8\"",
      },
      ARG_TABLE_ENDMARKER,
  };

  native_add_command_line_opts(opts);
}
NATIVE_TASK(add_options, PRE_BOOT_1, 10);

int sim_opt_link_port(void) { return link_port; }
const char *sim_opt_lr_state(void) { return lr_state; }
const char *sim_opt_bind(void) { return bind_addr != NULL ? bind_addr : "127.0.0.1"; }
const char *sim_opt_gnss(void) { return gnss_opt; }

/* ---- hex ---- */

static int nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = (char)tolower((unsigned char)c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int sim_hex_decode(const char *hex, uint8_t *out, int cap) {
  int n = 0;

  if (strcmp(hex, "-") == 0) {
    return 0;
  }
  for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
    int hi = nibble(hex[0]), lo = nibble(hex[1]);

    if (hi < 0 || lo < 0 || n >= cap) {
      return -1;
    }
    out[n++] = (uint8_t)(hi << 4 | lo);
  }
  return hex[0] == '\0' ? n : -1;
}

void sim_hex_encode(const uint8_t *in, int n, char *out) {
  static const char digits[] = "0123456789abcdef";

  for (int i = 0; i < n; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 15];
  }
  out[2 * n] = '\0';
}

/* ---- control connection ---- */

#define LINE_MAX_LEN 1200

static K_MUTEX_DEFINE(ctl_lock);
static int lfd = -1;
static int cfd = -1;
static char line[LINE_MAX_LEN];
static size_t line_n;
static struct k_thread ctl_thread;
K_THREAD_STACK_DEFINE(ctl_stack, 3072);

bool sim_ctl_connected(void) { return cfd >= 0; }

bool sim_ctl_send(const char *fmt, ...) {
  char buf[LINE_MAX_LEN];
  va_list ap;
  int n;
  bool ok = false;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return false;
  }
  n = MIN(n, (int)sizeof(buf) - 2);
  buf[n++] = '\n';
  k_mutex_lock(&ctl_lock, K_FOREVER);
  if (cfd >= 0) {
    if (lpwan_sim_write(cfd, buf, (size_t)n) == 0) {
      ok = true;
    } else {
      lpwan_sim_close(cfd);
      cfd = -1;
    }
  }
  k_mutex_unlock(&ctl_lock);
  return ok;
}

static void command(char *text) {
  char *argv[8];
  int argc = 0;
  char *save = NULL;

  for (char *t = strtok_r(text, " \t\r", &save); t != NULL && argc < 8; t = strtok_r(NULL, " \t\r", &save)) {
    argv[argc++] = t;
  }
  if (argc == 0) {
    return;
  }
  if (strcmp(argv[0], "lr") == 0) {
    sim_lr_command(argc - 1, argv + 1);
  } else if (strcmp(argv[0], "modem") == 0) {
    sim_modem_command(argc - 1, argv + 1);
#ifdef CONFIG_LPWAN_SHIELD_GNSS
  } else if (strcmp(argv[0], "gnss") == 0) {
    sim_gnss_command(argc - 1, argv + 1);
#endif
  } else if (strcmp(argv[0], "shield") == 0 && argc > 1 && strcmp(argv[1], "reset") == 0) {
    sim_shield_restart("reset on request of the harness");
  } else {
    LOG_WRN("unknown command '%s'", argv[0]);
  }
}

static void ctl_main(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);
  for (;;) {
    int fd = lpwan_sim_accept(lfd);
    char buf[256];
    int n;

    if (fd >= 0) {
      k_mutex_lock(&ctl_lock, K_FOREVER);
      lpwan_sim_close(cfd); /* a new harness replaces the old one */
      cfd = fd;
      line_n = 0;
      k_mutex_unlock(&ctl_lock);
      LOG_INF("harness connected");
      (void)sim_ctl_send("hello %d", (int)(k_uptime_get() / 1000));
    }
    if (cfd >= 0 && (n = lpwan_sim_read(cfd, buf, sizeof(buf))) != -2) {
      if (n <= 0) {
        k_mutex_lock(&ctl_lock, K_FOREVER);
        lpwan_sim_close(cfd);
        cfd = -1;
        k_mutex_unlock(&ctl_lock);
        LOG_INF("harness gone");
      }
      for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          line[line_n] = '\0';
          command(line);
          line_n = 0;
        } else if (line_n < sizeof(line) - 1) {
          line[line_n++] = buf[i];
        }
      }
      continue; /* more may be waiting */
    }
    k_msleep(1);
  }
}

/* The option holds the words of a gnss command; it applies before the first fix and survives a restart (the exec keeps
 * the command line). */
static void apply_gnss_option(void) {
#ifdef CONFIG_LPWAN_SHIELD_GNSS
  char *argv[8];
  int argc = 0;
  char *save = NULL;
  char *copy;

  if (gnss_opt == NULL) {
    return;
  }
  copy = strdup(gnss_opt);
  for (char *t = strtok_r(copy, " ,\t", &save); t != NULL && argc < 8; t = strtok_r(NULL, " ,\t", &save)) {
    argv[argc++] = t;
  }
  if (argc > 0) {
    sim_gnss_command(argc, argv);
  }
  free(copy);
#endif
}

void sim_shield_start(void) {
  LOG_INF("simulated LPWAN shield (native_sim)");
  apply_gnss_option();
  if (ctl_port <= 0) {
    LOG_WRN("no control port (-ctl_port): the LoRaWAN network is not reachable");
    return;
  }
  lfd = lpwan_sim_listen(sim_opt_bind(), ctl_port);
  if (lfd < 0) {
    LOG_ERR("cannot listen on %s:%d", sim_opt_bind(), ctl_port);
    return;
  }
  k_thread_create(&ctl_thread, ctl_stack, K_THREAD_STACK_SIZEOF(ctl_stack), ctl_main, NULL, NULL, NULL,
                  K_PRIO_PREEMPT(CONFIG_LPWAN_SHIELD_WORKER_PRIO), 0, K_NO_WAIT);
  k_thread_name_set(&ctl_thread, "ctl");
}

void sim_shield_restart(const char *why) {
  LOG_WRN("restart: %s", why);
  (void)sim_ctl_send("restart %s", why);
  sys_reboot(SYS_REBOOT_COLD); /* the process executes itself again; the sockets are close-on-exec */
}
