/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/stream.h"

#include "sensei_link/crc.h"

size_t sensei_stream_encode(uint8_t channel, const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
  uint16_t crc = sensei_crc16(SENSEI_CRC16_INIT, &channel, 1);
  uint8_t trailer[2];
  sensei_cobs_seg_t segs[3];
  size_t n;

  crc = sensei_crc16(crc, payload, len);
  trailer[0] = (uint8_t)(crc & 0xFF);
  trailer[1] = (uint8_t)(crc >> 8);
  segs[0].data = &channel;
  segs[0].len = 1;
  segs[1].data = payload;
  segs[1].len = len;
  segs[2].data = trailer;
  segs[2].len = 2;

  n = sensei_cobs_encode_segs(segs, 3, out, cap);
  if (n == 0 || n >= cap) {
    return 0;
  }
  out[n] = 0;
  return n + 1;
}

void sensei_stream_rx_init(sensei_stream_rx_t *rx, uint8_t *buf, size_t cap) {
  rx->buf = buf;
  rx->cap = cap;
  rx->len = 0;
  rx->overflow = false;
  rx->channel = 0;
  rx->payload = 0;
  rx->payload_len = 0;
  rx->n_frames = 0;
  rx->n_crc_err = 0;
  rx->n_format_err = 0;
  rx->n_overflow = 0;
}

sensei_stream_status_t sensei_stream_rx_feed(sensei_stream_rx_t *rx, uint8_t byte) {
  size_t n;
  uint16_t crc;
  uint16_t got;

  if (byte != 0) {
    if (rx->overflow) {
      return SENSEI_STREAM_NONE;
    }
    if (rx->len >= rx->cap) {
      rx->overflow = true;
      return SENSEI_STREAM_NONE;
    }
    rx->buf[rx->len++] = byte;
    return SENSEI_STREAM_NONE;
  }

  /* delimiter */
  if (rx->overflow) {
    rx->overflow = false;
    rx->len = 0;
    rx->n_overflow++;
    return SENSEI_STREAM_ERR_OVERFLOW;
  }
  if (rx->len == 0) {
    return SENSEI_STREAM_NONE; /* empty frame, for example a flush byte */
  }
  if (!sensei_cobs_decode(rx->buf, rx->len, rx->buf, rx->cap, &n) || n < 3) {
    rx->len = 0;
    rx->n_format_err++;
    return SENSEI_STREAM_ERR_FORMAT;
  }
  rx->len = 0;
  crc = sensei_crc16(SENSEI_CRC16_INIT, rx->buf, n - 2);
  got = (uint16_t)(rx->buf[n - 2] | (rx->buf[n - 1] << 8));
  if (crc != got) {
    rx->n_crc_err++;
    return SENSEI_STREAM_ERR_CRC;
  }
  rx->channel = rx->buf[0];
  rx->payload = rx->buf + 1;
  rx->payload_len = n - 3;
  rx->n_frames++;
  return SENSEI_STREAM_FRAME;
}
