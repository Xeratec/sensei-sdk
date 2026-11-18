/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file g9ota_mram_ops.h
 * @brief The MRAM operations of the writer (`struct g9ota_flash`) on the GAP SDK (pi_flash), without the parts that
 *        only an application needs (hand-off arming, readfs of the slot: g9ota_mram.h). The SSBL recovery and the
 *        application share this file.
 */

#ifndef SENSEI_GAP9_G9OTA_MRAM_OPS_H_
#define SENSEI_GAP9_G9OTA_MRAM_OPS_H_

#include <stdbool.h>

#include "sensei_gap9/g9ota.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the MRAM (once; later calls return 0). Returns -EIO if it does not open. */
int g9ota_mram_open(void);

/** The operations of the writer: blocking read (any offset and length), sector erase, 16 byte aligned program. They
 * fail with -ENODEV before g9ota_mram_open() succeeded. The context argument is not used. */
const struct g9ota_flash *g9ota_mram_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_GAP9_G9OTA_MRAM_OPS_H_ */
