/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Used inside lib/sensei_gap9/zephyr only: shell access to the link. */

#ifndef SENSEI_GAP9_PRIV_H_
#define SENSEI_GAP9_PRIV_H_

#include "sensei_gap9/gap9_zephyr.h"

/** The app's ping (gap9_link_hooks.ping), or -ENOTSUP. */
int gap9_link_ping(uint32_t timeout_ms, uint32_t *duration_us, sensei_Version *ver);

#endif
