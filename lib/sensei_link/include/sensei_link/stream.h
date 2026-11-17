/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Framing for byte streams (USB CDC, UART, BLE NUS).
 *
 *   frame = COBS( channel(1) | payload | crc16(2, little endian) ) | 0x00
 *
 * The CRC covers channel and payload. A receiver that loses sync drops bytes up to the next 0x00.
 */
#ifndef SENSEI_LINK_STREAM_H
#define SENSEI_LINK_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_link/cobs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest frame on the wire (with delimiter) for a payload of `payload_max` bytes. */
#define SENSEI_STREAM_FRAME_MAX(payload_max) (SENSEI_COBS_MAX_ENCODED((payload_max) + 3u) + 1u)

/* Encode a frame into `out`. Returns the frame length including the delimiter, or 0 if `cap` is too
 * small. */
size_t sensei_stream_encode(uint8_t channel, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

typedef enum {
  SENSEI_STREAM_NONE = 0,         /* frame not complete yet */
  SENSEI_STREAM_FRAME = 1,        /* a valid frame is available in the receiver */
  SENSEI_STREAM_ERR_CRC = -1,     /* frame dropped, CRC mismatch */
  SENSEI_STREAM_ERR_FORMAT = -2,  /* frame dropped, bad COBS or too short */
  SENSEI_STREAM_ERR_OVERFLOW = -3 /* frame dropped, longer than the buffer */
} sensei_stream_status_t;

typedef struct {
  uint8_t *buf; /* caller storage, SENSEI_STREAM_FRAME_MAX(max payload) bytes */
  size_t cap;
  size_t len;
  bool overflow;
  /* valid after SENSEI_STREAM_FRAME, until the next feed */
  uint8_t channel;
  const uint8_t *payload;
  size_t payload_len;
  /* counters */
  uint32_t n_frames;
  uint32_t n_crc_err;
  uint32_t n_format_err;
  uint32_t n_overflow;
} sensei_stream_rx_t;

void sensei_stream_rx_init(sensei_stream_rx_t *rx, uint8_t *buf, size_t cap);

/* Feed one received byte. */
sensei_stream_status_t sensei_stream_rx_feed(sensei_stream_rx_t *rx, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif
