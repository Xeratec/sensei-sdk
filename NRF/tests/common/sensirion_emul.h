/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Helpers of the Sensirion I2C emulators (SCD41, SGP41).
 *
 * Protocol (Sensirion datasheets, chapter "I2C communication", checksum section): a command is 16 bits, big
 * endian. Data go in 16-bit words, each followed by a CRC-8 (polynomial 0x31, init 0xFF, no reflection, no final
 * XOR). The sensor does not acknowledge a data word with a wrong CRC, and it does not acknowledge anything while
 * it executes a command (the execution times are in the datasheet command tables). */

#ifndef SENSEI_TEST_SENSIRION_EMUL_H_
#define SENSEI_TEST_SENSIRION_EMUL_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#define SENSIRION_EMUL_LOG_LEN 64

static inline uint8_t sensirion_emul_crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;

  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

/* Words with their CRC into out (3 bytes per word). Returns the byte count. */
static inline size_t sensirion_emul_put_words(uint8_t *out, const uint16_t *words, size_t n) {
  for (size_t i = 0; i < n; i++) {
    out[3 * i] = (uint8_t)(words[i] >> 8);
    out[3 * i + 1] = (uint8_t)words[i];
    out[3 * i + 2] = sensirion_emul_crc8(&out[3 * i], 2);
  }
  return 3 * n;
}

/* Command and argument words of a write. Returns the number of words, -1 on a wrong length or CRC. */
static inline int sensirion_emul_parse(const uint8_t *buf, size_t len, uint16_t *cmd, uint16_t *args, size_t max_args) {
  size_t n;

  if (len < 2 || (len - 2) % 3 != 0) {
    return -1;
  }
  n = (len - 2) / 3;
  if (n > max_args) {
    return -1;
  }
  *cmd = (uint16_t)((buf[0] << 8) | buf[1]);
  for (size_t i = 0; i < n; i++) {
    const uint8_t *w = &buf[2 + 3 * i];

    if (sensirion_emul_crc8(w, 2) != w[2]) {
      return -1;
    }
    args[i] = (uint16_t)((w[0] << 8) | w[1]);
  }
  return (int)n;
}

static inline int64_t sensirion_emul_now_us(void) { return k_ticks_to_us_floor64(k_uptime_ticks()); }

#endif /* SENSEI_TEST_SENSIRION_EMUL_H_ */
