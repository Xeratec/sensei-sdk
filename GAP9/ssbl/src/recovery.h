/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Recovery of the SSBL (SENSEI_SSBL_RECOVERY, README.md, lib/sensei_gap9/OTA.md "Recovery"): when no slot boots, the
 * SSBL runs the update writer and serves it to the nRF5340 over the SPI link. */

#ifndef SENSEI_SSBL_RECOVERY_H_
#define SENSEI_SSBL_RECOVERY_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_gap9/g9rec.h"

/** Is the recovery strap high (pad 57, pull-down)? Without SENSEI_SSBL_STRAP: false, and the pad is not touched.
 * Untested on hardware. */
bool recovery_strap(void);

/** Serve the recovery jobs with the writer `r` (keep slot, never written). The production build does not return (the
 * nRF5340 powers GAP9 off or on again); the GVSoC test build (SENSEI_SSBL_RECOVERY_TEST) feeds the jobs from the
 * keep slot and returns 0 when it is done. Returns a negative value if the link or the feed cannot start. */
int recovery_serve(struct g9rec *r, uint32_t reason);

/** Set up the writer for `keep` (the slot on the slot line) and serve. `reason`: G9L_RECOVERY_BOTH_BAD or
 * G9L_RECOVERY_FORCED. Returns what recovery_serve() returns, or a negative value if the MRAM or the layout is wrong.
 */
int recovery_run(uint8_t keep, uint32_t reason);

#endif /* SENSEI_SSBL_RECOVERY_H_ */
