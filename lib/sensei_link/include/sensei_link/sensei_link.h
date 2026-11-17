/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Constants of the SENSEI link layer: framing version, SPI frame types and limits.
 */
#ifndef SENSEI_LINK_H
#define SENSEI_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version of the framing (stream, SPI, fragments). Bump on incompatible changes. */
#define SENSEI_LINK_VERSION 1

/* Largest payload of an SPI frame. An app with larger messages defines it before the first include
 * of a sensei_link header (the same value on both ends of the link). */
#ifndef SENSEI_SPI_MAX_PAYLOAD
#define SENSEI_SPI_MAX_PAYLOAD 8192u
#endif

/* Type byte of an SPI frame. 0x10 to 0x1F: GAP9 link (see sensei_gap9), 0x20 to 0x2F: LPWAN shield. */
typedef enum {
  SENSEI_SPI_GAP9_JOB = 0x10,       /* job, nRF5340 to GAP9 */
  SENSEI_SPI_GAP9_RESULT = 0x11,    /* result, GAP9 to nRF5340 */
  SENSEI_SPI_LPWAN_REQUEST = 0x20,  /* LpwanRequest, nRF5340 to nRF9151 */
  SENSEI_SPI_LPWAN_RESPONSE = 0x21, /* LpwanResponse */
  SENSEI_SPI_LPWAN_EVENT = 0x22,    /* LpwanEvent */
} sensei_spi_type_t;

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LINK_H */
