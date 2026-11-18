/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sim_shield.h
 * @brief The shield app on native_sim: command line, control connection to the test harness, restart.
 *
 * The harness (the fake LoRaWAN network server and the test) connects to the control port of the shield and talks
 * in text lines; see README.md in this folder for the commands. Everything in the shield that stands for hardware
 * or the network (LR1121, modem) takes its commands and reports its traffic through this connection.
 */

#ifndef SENSEI_SHIELD_SIM_H_
#define SENSEI_SHIELD_SIM_H_

#include <stdbool.h>
#include <stdint.h>

/** Start the control connection. Call first in main(). */
void sim_shield_start(void);

/** Send one line (a newline is added) to the harness. Dropped if nobody is connected. Returns false if dropped. */
bool sim_ctl_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** True while a harness is connected. */
bool sim_ctl_connected(void);

/** Port on which the node simulator connects (-link_port), 0: not given. */
int sim_opt_link_port(void);

/** File that holds what the LR1121 keeps in its flash (-lr_state), NULL: none (lost at a restart). */
const char *sim_opt_lr_state(void);

/** Address to listen on (-bind). */
const char *sim_opt_bind(void);

/** Restart the process: the shield has lost its power (what a reset or a power cycle does on the board). */
void sim_shield_restart(const char *why);

/** Commands of the harness: "lr <...>" (sim_lr1121.c), "modem <...>" (sim_modem.c), "gnss <...>" (sim_gnss.c). */
void sim_lr_command(int argc, char **argv);
void sim_modem_command(int argc, char **argv);
void sim_gnss_command(int argc, char **argv);

/** The fake modem (sim_modem.c) and the fake GNSS (sim_gnss.c) tell each other: GNSS is active in the functional mode;
 *  a functional mode change took GNSS away; the RRC state changed (the modem gives LTE priority). */
bool sim_modem_gnss_active(void);
void sim_gnss_deactivated(void);
void sim_gnss_rrc(bool connected);

/** The option -gnss="<words of the gnss command>": the scenario that survives a restart of the shield. NULL: none. */
const char *sim_opt_gnss(void);

/** Hex text to bytes and back. Returns the number of bytes (or -1 for bad text). */
int sim_hex_decode(const char *hex, uint8_t *out, int cap);
void sim_hex_encode(const uint8_t *in, int n, char *out); /* out: 2 * n + 1 bytes */

#endif /* SENSEI_SHIELD_SIM_H_ */
