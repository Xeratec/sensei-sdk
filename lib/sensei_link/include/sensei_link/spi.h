/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * SPI link frame (GAP9 and nRF9151), all fields little endian:
 *
 *   0       1        2     3      4..5   6..7
 *   magic   version  type  flags  len    crc16 of the payload
 *   0x5E    1
 *
 * The header is followed by `len` payload bytes (a protobuf message).
 */
#ifndef SENSEI_LINK_SPI_H
#define SENSEI_LINK_SPI_H

#include <stddef.h>
#include <stdint.h>

#include "sensei_link/sensei_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_SPI_MAGIC 0x5Eu
#define SENSEI_SPI_HEADER_SIZE 8u

/* flags */
#define SENSEI_SPI_FLAG_ERROR 0x01u /* the sender could not handle the previous frame */
#define SENSEI_SPI_FLAG_MORE 0x02u  /* more frames of the same message follow (reserved) */

typedef struct {
  uint8_t version;
  uint8_t type;
  uint8_t flags;
  uint16_t len;
  uint16_t crc;
} sensei_spi_header_t;

typedef enum {
  SENSEI_SPI_OK = 0,
  SENSEI_SPI_ERR_MAGIC = -1,
  SENSEI_SPI_ERR_VERSION = -2,
  SENSEI_SPI_ERR_LEN = -3, /* longer than SENSEI_SPI_MAX_PAYLOAD or than the buffer */
  SENSEI_SPI_ERR_CRC = -4,
  SENSEI_SPI_ERR_SHORT = -5, /* fewer bytes than the header says */
} sensei_spi_status_t;

/* Write the 8 header bytes for a payload. */
void sensei_spi_header_pack(uint8_t out[SENSEI_SPI_HEADER_SIZE], uint8_t type, uint8_t flags, const uint8_t *payload,
                            size_t len);

/* Parse and check magic, version and length (not the CRC). */
sensei_spi_status_t sensei_spi_header_parse(const uint8_t in[SENSEI_SPI_HEADER_SIZE], sensei_spi_header_t *hdr);

/* Check the payload CRC against a parsed header. */
sensei_spi_status_t sensei_spi_check_payload(const sensei_spi_header_t *hdr, const uint8_t *payload);

/* Header and payload in one buffer. Returns the frame length or 0 if `cap` is too small. */
size_t sensei_spi_frame_encode(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len, uint8_t *out,
                               size_t cap);

/* Parse a whole frame; `*payload` points into `buf`. Extra bytes after the frame are ignored. */
sensei_spi_status_t sensei_spi_frame_decode(const uint8_t *buf, size_t len, sensei_spi_header_t *hdr,
                                            const uint8_t **payload);

#ifdef __cplusplus
}
#endif

#endif
