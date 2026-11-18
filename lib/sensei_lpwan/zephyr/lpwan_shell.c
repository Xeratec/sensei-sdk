/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_shell.c
 * @brief Shell: sensei lpwan status | power | join | connect | disconnect | gnss | provision.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "lpwan_priv.h"
#include "sensei_lpwan/lpwan.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv) {
  struct lpwan_info i;

  sensei_lpwan_get_info(&i);
  if (sensei_lpwan_shield_state() == LPWAN_SHIELD_ABSENT) {
    shell_print(sh, "shield absent (the expander does not answer): the node sends over BLE; reboot to look again");
    return 0;
  }
  shell_print(sh, "shield %s, link %s, state %s, tech %s%s", i.shield_on ? "on" : "off", i.link_up ? "up" : "down",
              lpwan_state_name(i.state), lpwan_tech_name(i.tech),
              i.tech == LPWAN_TECH_LTE ? (i.lte_mode_nb ? " (NB-IoT)" : " (LTE-M)") : "");
  shell_print(sh, "nRF9151 firmware '%s', modem '%s'", i.fw, i.modem);
  shell_print(sh, "transfers %u, bad frames %u, retries %u, events %u", i.transfers, i.bad_frames, i.retries, i.events);
  shell_print(sh, "uplinks ok %u, failed %u, last result %d", i.uplink_ok, i.uplink_fail, i.last_result);
  shell_print(sh, "LTE answers %u, sent unconfirmed %u", i.lte_acked, i.lte_noack);
  {
    struct sensei_lpwan_lte_info e;

    if (sensei_lpwan_lte_info(&e) == 0) {
      shell_print(
          sh,
          "LTE evaluation %u s ago: energy %d (1 bad ... 5 excellent), RSRP %d dBm, CE level %u, RAI of the cell %d",
          e.age_s, (int)e.energy, (int)e.rsrp_dbm, (unsigned)e.ce_level, (int)e.rai);
    }
  }
  shell_print(sh, "requests failed in a row %u, shield power cycles after a lost link %u", i.failures, i.recoveries);
  if (i.proto_mismatch) {
    shell_error(sh, "the nRF9151 speaks another protocol version: update the shield firmware, reboot");
  }
  if (i.provisioned_pending) {
    shell_print(sh, "LoRaWAN keys waiting to be written to the LR1121");
  }
  shell_print(sh, "SPI_A bus %s", sensei_lpwan_spi_in_use() ? "in use" : "free");
  return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv) {
  bool on = strcmp(argv[1], "on") == 0;

  if (!on && strcmp(argv[1], "off") != 0) {
    shell_error(sh, "power on|off");
    return -EINVAL;
  }
  return sensei_lpwan_cmd_power(on);
}

static int cmd_join(const struct shell *sh, size_t argc, char **argv) {
  return sensei_lpwan_cmd_connect(sensei_Radio_RADIO_LORA);
}

static int cmd_connect(const struct shell *sh, size_t argc, char **argv) {
  if (strcmp(argv[1], "ltem") == 0) {
    return sensei_lpwan_cmd_connect(sensei_Radio_RADIO_LTEM);
  }
  if (strcmp(argv[1], "nbiot") == 0) {
    return sensei_lpwan_cmd_connect(sensei_Radio_RADIO_NBIOT);
  }
  if (strcmp(argv[1], "lora") == 0) {
    return sensei_lpwan_cmd_connect(sensei_Radio_RADIO_LORA);
  }
  shell_error(sh, "connect lora|ltem|nbiot");
  return -EINVAL;
}

static int cmd_disconnect(const struct shell *sh, size_t argc, char **argv) { return sensei_lpwan_cmd_disconnect(); }

/* The keys are typed on the shell: they stay in the shell history and in what the transport logs. Use it on
 * the bench, not in the field; the host link (lpwan_lora_provision) does not echo them. */
static int cmd_provision(const struct shell *sh, size_t argc, char **argv) {
  sensei_LoraProvision k = sensei_LoraProvision_init_zero;
  int err;

  if (argc != 4) {
    shell_error(sh, "provision <dev_eui 16 hex> <join_eui 16 hex> <app_key 32 hex>");
    return -EINVAL;
  }
  if (strlen(argv[1]) != 16 || strlen(argv[2]) != 16 || strlen(argv[3]) != 32 ||
      hex2bin(argv[1], 16, k.dev_eui, 8) != 8 || hex2bin(argv[2], 16, k.join_eui, 8) != 8 ||
      hex2bin(argv[3], 32, k.app_key, 16) != 16) {
    shell_error(sh, "keys must be hex of the right length");
    return -EINVAL;
  }
  err = sensei_lpwan_lora_provision(&k);
  memset(&k, 0, sizeof(k));
  if (err) {
    shell_error(sh, "provision failed: %d", err);
  } else {
    shell_print(sh, "keys accepted, joining (see status)");
  }
  return err;
}

/* The result arrives later on the hook work queue: print it on the shell that asked. */
static const struct shell *gnss_sh;

static void gnss_done(int err, const sensei_GnssFixResult *r, void *user) {
  ARG_UNUSED(user);
  if (err == 0 && r->status == sensei_GnssFixStatus_GNSS_FIX_OK) {
    shell_print(gnss_sh, "GNSS fix after %u ms: lat %d lon %d (deg x 1e7), alt %d m, +-%u m, %u satellites, time %u",
                (unsigned)r->ttff_ms, (int)r->lat_e7, (int)r->lon_e7, (int)r->alt_m, (unsigned)r->acc_m,
                (unsigned)r->sats, (unsigned)r->unix_time);
  } else {
    shell_print(gnss_sh, "GNSS: %s (err %d)", lpwan_gnss_status_name(r->status), err);
  }
}

static int cmd_gnss(const struct shell *sh, size_t argc, char **argv) {
  int err;

  gnss_sh = sh;
  err = sensei_lpwan_gnss_fix(argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 0, gnss_done, NULL);
  if (err) {
    shell_error(sh, "GNSS fix not started (%d)", err);
  } else {
    shell_print(sh, "GNSS fix asked for (needs sky view and the antenna supply): the result follows");
  }
  return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_lpwan, SHELL_CMD(status, NULL, "Shield, link and radio state", cmd_status),
    SHELL_CMD_ARG(power, NULL, "power on|off", cmd_power, 2, 0), SHELL_CMD(join, NULL, "Join LoRaWAN", cmd_join),
    SHELL_CMD_ARG(connect, NULL, "connect lora|ltem|nbiot", cmd_connect, 2, 0),
    SHELL_CMD(disconnect, NULL, "Release the active radio", cmd_disconnect),
    SHELL_CMD_ARG(gnss, NULL, "gnss [timeout_s]: one GNSS fix of the nRF9151", cmd_gnss, 1, 1),
    SHELL_CMD_ARG(provision, NULL, "provision <dev_eui> <join_eui> <app_key> (hex)", cmd_provision, 4, 0),
    SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((sensei), lpwan, &sub_lpwan, "LPWAN shield (LoRaWAN, LTE-M, NB-IoT)", NULL, 1, 0);
