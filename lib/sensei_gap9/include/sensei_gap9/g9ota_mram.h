/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9ota_mram.h
 * @brief GAP9 side of the firmware update: the MRAM backend of the writer (g9ota.h), the slot this
 *        application runs from, and the readfs of that slot. Built into GAP9 apps with the GAP SDK (see
 *        sensei_gap9.cmake). The rules are in OTA.md.
 *
 * The slot this application runs from is the one in the hand-off record of the SSBL (GAP9/ssbl), and only
 * that. The writer is armed with a record that names a slot holding a complete image: the other slot and the
 * checkpoint sectors are then free to write. Without one (the single-image layout of `make flash`, a JTAG
 * load, a reset without the SSBL) nothing is written.
 */

#ifndef SENSEI_GAP9_G9OTA_MRAM_H_
#define SENSEI_GAP9_G9OTA_MRAM_H_

#include <stdbool.h>
#include <stdint.h>

#include "sensei_gap9/g9ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Volume of the partition table that holds the placeholder readfs partition (GAP9/layouts/sensei_ab.json). */
#define G9OTA_READFS_VOLUME "app"

/**
 * Open the MRAM, find the slot this application runs from and set up the writer. Call once at start-up, before
 * any job. `kind` is the board id of the images (the kind TLV), valid for the life of the program. `test_area`
 * is for the local test on a single-image layout: the writer then uses this area (slots, checkpoint sectors) as
 * if it ran from slot A, and writes nowhere else; NULL for the real A/B layout. Returns 0, or a negative value
 * if the MRAM does not open (the update jobs then answer an error).
 */
int g9ota_mram_init(const char *kind, const struct g9ota_cfg *test_area);

/** The writer, or NULL before g9ota_mram_init() succeeded or when it is not armed. */
struct g9ota *g9ota_mram_get(void);

/** The slot (0 = A, 1 = B) this application runs from, and whether it runs from a slot at all. */
uint8_t g9ota_mram_booted_slot(void);
bool g9ota_mram_from_slot(void);

/**
 * MRAM offset of the readfs of this application's slot, 0xFFFFFFFF when it does not run from a slot. The
 * wrapper of pi_fs_conf_init in this file uses it: link with `-Wl,--wrap=pi_fs_conf_init`
 * (SENSEI_GAP9_OTA_LINK_OPTIONS of sensei_gap9.cmake).
 */
uint32_t g9ota_mram_readfs_offset(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAP9_G9OTA_MRAM_H_ */
