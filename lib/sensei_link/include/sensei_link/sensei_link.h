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

/* Version of the framing (stream, SPI, fragments). Bump on incompatible changes.
 * 2: the SPI frame CRC covers header bytes 0 to 5 and the payload (was: payload only). */
#define SENSEI_LINK_VERSION 2

/* Largest payload of an SPI frame, the same on both ends of a link. Set it for the whole build, never
 * per file: Zephyr CONFIG_SENSEI_SPI_MAX_PAYLOAD, CMake -DSENSEI_SPI_MAX_PAYLOAD=<n> (sensei_link.cmake
 * adds it to SENSEI_LINK_DEFINITIONS). Default 8192. */
#ifndef SENSEI_SPI_MAX_PAYLOAD
#ifdef CONFIG_SENSEI_SPI_MAX_PAYLOAD
#define SENSEI_SPI_MAX_PAYLOAD CONFIG_SENSEI_SPI_MAX_PAYLOAD
#else
#define SENSEI_SPI_MAX_PAYLOAD 8192u
#endif
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
