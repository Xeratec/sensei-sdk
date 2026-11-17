/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * SHA-256 (FIPS 180-4), small and portable: no allocation, no OS, no crypto hardware. For checking an image that
 * arrives in pieces (see sensei_lpwan/lpwan_dfu.h). Not constant time: it hashes public data only.
 */
#ifndef SENSEI_LINK_SHA256_H
#define SENSEI_LINK_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_SHA256_LEN 32u

typedef struct {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t total; /* bytes hashed so far */
} sensei_sha256_t;

void sensei_sha256_init(sensei_sha256_t *c);
void sensei_sha256_update(sensei_sha256_t *c, const uint8_t *data, size_t len);
/* Write the digest. The context is not usable again until sensei_sha256_init(). */
void sensei_sha256_final(sensei_sha256_t *c, uint8_t out[SENSEI_SHA256_LEN]);
/* Hash `len` bytes of 0xFF (a skipped, erased region). */
void sensei_sha256_update_ff(sensei_sha256_t *c, size_t len);

#ifdef __cplusplus
}
#endif

#endif
