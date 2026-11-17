/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/cobs.h"

size_t sensei_cobs_encode_segs(const sensei_cobs_seg_t *segs, size_t nseg, uint8_t *out, size_t cap) {
  size_t o = 1;        /* next free output byte */
  size_t code_pos = 0; /* where the current block's code goes */
  uint8_t code = 1;

  if (cap < 1) {
    return 0;
  }
  for (size_t s = 0; s < nseg; s++) {
    for (size_t i = 0; i < segs[s].len; i++) {
      uint8_t b = segs[s].data[i];
      if (b != 0) {
        if (o >= cap) {
          return 0;
        }
        out[o++] = b;
        code++;
      }
      if (b == 0 || code == 0xFF) {
        out[code_pos] = code;
        if (o >= cap) {
          return 0;
        }
        code_pos = o++;
        code = 1;
      }
    }
  }
  out[code_pos] = code;
  return o;
}

size_t sensei_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
  sensei_cobs_seg_t seg = {in, len};
  return sensei_cobs_encode_segs(&seg, 1, out, cap);
}

bool sensei_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *out_len) {
  size_t i = 0;
  size_t o = 0;

  if (len == 0) {
    return false;
  }
  while (i < len) {
    uint8_t code = in[i++];
    size_t n = (size_t)code - 1u;
    if (code == 0 || n > len - i || o + n > cap) {
      return false;
    }
    for (size_t k = 0; k < n; k++) {
      if (in[i] == 0) {
        return false;
      }
      out[o++] = in[i++];
    }
    if (code != 0xFF && i < len) {
      if (o >= cap) {
        return false;
      }
      out[o++] = 0;
    }
  }
  *out_len = o;
  return true;
}
