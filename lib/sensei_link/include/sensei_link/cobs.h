/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Consistent Overhead Byte Stuffing. The encoded data contains no 0x00.
 */
#ifndef SENSEI_LINK_COBS_H
#define SENSEI_LINK_COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest encoded size of `len` input bytes. */
#define SENSEI_COBS_MAX_ENCODED(len) ((len) + (len) / 254u + 1u)

/* A piece of input; encoding runs over several pieces as if they were concatenated. */
typedef struct {
  const uint8_t *data;
  size_t len;
} sensei_cobs_seg_t;

/* Encode. Returns the encoded length (at least 1), or 0 if `cap` is too small. No delimiter added.
 */
size_t sensei_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);
size_t sensei_cobs_encode_segs(const sensei_cobs_seg_t *segs, size_t nseg, uint8_t *out, size_t cap);

/* Decode. `out` may equal `in` (in place). Returns false on malformed input or if `cap` is too
 * small. */
bool sensei_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
