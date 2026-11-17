/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/crc.h"

uint16_t sensei_crc16(uint16_t crc, const uint8_t *data, size_t len) {
  while (len--) {
    crc ^= (uint16_t)((unsigned)*data++ << 8);
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x8000u) ? (uint16_t)(((unsigned)crc << 1) ^ 0x1021u) : (uint16_t)((unsigned)crc << 1);
    }
  }
  return crc;
}

uint32_t sensei_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
  }
  return ~crc;
}
