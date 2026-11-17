/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Shell: sensei gap9 [on|off|ping|status] */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "gap9_priv.h"

static const char *const state_name[] = {"off", "booting", "running", "restarting"};

static int cmd_status(const struct shell *sh) {
  struct gap9_link_status st;

  gap9_link_status_get(&st);
  shell_print(sh, "state %s, busy %d, errors %u, GAP9 firmware %u.%u.%u", state_name[st.state], st.busy, st.errors,
              st.version.major, st.version.minor, st.version.patch);
  return 0;
}

static int cmd_gap9(const struct shell *sh, size_t argc, char **argv) {
  uint32_t us;
  sensei_Version v;

  if (argc < 2 || strcmp(argv[1], "status") == 0) {
    return cmd_status(sh);
  }
  if (strcmp(argv[1], "off") == 0) {
    gap9_link_power_off_now();
    shell_print(sh, "GAP9 off");
    return 0;
  }
  if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "ping") == 0) {
    /* "on" is a ping too: GAP9 boots, answers, and is powered off after the idle delay. */
    int ret = gap9_link_ping(15000, &us, &v);
    if (ret != 0) {
      shell_error(sh, "no answer (%d)", ret);
      return ret;
    }
    shell_print(sh, "GAP9 firmware %u.%u.%u, job %u us", v.major, v.minor, v.patch, us);
    return 0;
  }
  shell_error(sh, "usage: sensei gap9 [on|off|ping|status]");
  return -EINVAL;
}

SHELL_SUBCMD_ADD((sensei), gap9, NULL, "gap9 [on|off|ping|status]", cmd_gap9, 1, 1);
