/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_sim_master.h
 * @brief Client side of the simulated shield link (native_sim): what the SPI master, BUSY, EVENT and the shield power
 *        are on the board. See sensei_lpwan/lpwan_sim.h for the protocol.
 */

#ifndef SENSEI_LPWAN_SIM_MASTER_H_
#define SENSEI_LPWAN_SIM_MASTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** True if the simulator was told where the shield is (`-lpwan_shield=<host>:<port>` or the target option). Without it
 *  the simulated node has no shield: the client does not start (like a node without the expander). */
bool lpwan_sim_master_configured(void);

/** Start the link thread. `on_event` is called (link thread) when EVENT goes high. Returns 0 or a negative errno. */
int lpwan_sim_master_init(void (*on_event)(void));

/**
 * Power the shield: connect to it (the connection is retried until it works), or cut the connection. Returns 0, or
 * -ENODEV if the simulator was not told where the shield is (`-lpwan_shield=<host>:<port>`).
 */
int lpwan_sim_master_power(bool on);

/** BUSY level: true while the shield is not ready (also while it is off or not connected yet). */
bool lpwan_sim_master_busy(void);

/** EVENT level. */
bool lpwan_sim_master_event(void);

/** One transfer. Returns 0 or a negative errno (-ENOTCONN, -ETIMEDOUT). */
int lpwan_sim_master_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

#endif /* SENSEI_LPWAN_SIM_MASTER_H_ */
