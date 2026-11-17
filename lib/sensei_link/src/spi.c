/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/spi.h"

#include <string.h>

#include "sensei_link/crc.h"

/* CRC over header bytes 0 to 5 (magic ... len) and the payload. */
static uint16_t frame_crc(const uint8_t head[6], const uint8_t *payload, size_t len) {
  return sensei_crc16(sensei_crc16(SENSEI_CRC16_INIT, head, 6), payload, len);
}

void sensei_spi_header_pack(uint8_t out[SENSEI_SPI_HEADER_SIZE], uint8_t type, uint8_t flags, const uint8_t *payload,
                            size_t len) {
  uint16_t crc;

  out[0] = SENSEI_SPI_MAGIC;
  out[1] = SENSEI_LINK_VERSION;
  out[2] = type;
  out[3] = flags;
  out[4] = (uint8_t)(len & 0xFF);
  out[5] = (uint8_t)(len >> 8);
  crc = frame_crc(out, payload, len);
  out[6] = (uint8_t)(crc & 0xFF);
  out[7] = (uint8_t)(crc >> 8);
}

sensei_spi_status_t sensei_spi_header_parse(const uint8_t in[SENSEI_SPI_HEADER_SIZE], sensei_spi_header_t *hdr) {
  if (in[0] != SENSEI_SPI_MAGIC) {
    return SENSEI_SPI_ERR_MAGIC;
  }
  if (in[1] != SENSEI_LINK_VERSION) {
    return SENSEI_SPI_ERR_VERSION;
  }
  hdr->version = in[1];
  hdr->type = in[2];
  hdr->flags = in[3];
  hdr->len = (uint16_t)(in[4] | (in[5] << 8));
  hdr->crc = (uint16_t)(in[6] | (in[7] << 8));
  if (hdr->len > SENSEI_SPI_MAX_PAYLOAD) {
    return SENSEI_SPI_ERR_LEN;
  }
  return SENSEI_SPI_OK;
}

sensei_spi_status_t sensei_spi_check_payload(const sensei_spi_header_t *hdr, const uint8_t *payload) {
  const uint8_t head[6] = {SENSEI_SPI_MAGIC,        hdr->version, hdr->type, hdr->flags, (uint8_t)(hdr->len & 0xFF),
                           (uint8_t)(hdr->len >> 8)};

  if (frame_crc(head, payload, hdr->len) != hdr->crc) {
    return SENSEI_SPI_ERR_CRC;
  }
  return SENSEI_SPI_OK;
}

size_t sensei_spi_frame_encode(uint8_t type, uint8_t flags, const uint8_t *payload, size_t len, uint8_t *out,
                               size_t cap) {
  if (len > SENSEI_SPI_MAX_PAYLOAD || cap < SENSEI_SPI_HEADER_SIZE + len) {
    return 0;
  }
  memmove(out + SENSEI_SPI_HEADER_SIZE, payload, len);
  sensei_spi_header_pack(out, type, flags, out + SENSEI_SPI_HEADER_SIZE, len);
  return SENSEI_SPI_HEADER_SIZE + len;
}

sensei_spi_status_t sensei_spi_frame_decode(const uint8_t *buf, size_t len, sensei_spi_header_t *hdr,
                                            const uint8_t **payload) {
  sensei_spi_status_t st;

  if (len < SENSEI_SPI_HEADER_SIZE) {
    return SENSEI_SPI_ERR_SHORT;
  }
  st = sensei_spi_header_parse(buf, hdr);
  if (st != SENSEI_SPI_OK) {
    return st;
  }
  if (len < SENSEI_SPI_HEADER_SIZE + (size_t)hdr->len) {
    return SENSEI_SPI_ERR_SHORT;
  }
  *payload = buf + SENSEI_SPI_HEADER_SIZE;
  return sensei_spi_check_payload(hdr, *payload);
}
