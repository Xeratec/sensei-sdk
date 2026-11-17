/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */
#ifndef SENSEI_LINK_CRC_H
#define SENSEI_LINK_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_CRC16_INIT 0xFFFFu

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor.
 * Check value of "123456789": 0x29B1. Pass the previous result as `crc` to continue. */
uint16_t sensei_crc16(uint16_t crc, const uint8_t *data, size_t len);

/* CRC-32 (IEEE 802.3, as zlib.crc32). Check value of "123456789": 0xCBF43926. */
uint32_t sensei_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
