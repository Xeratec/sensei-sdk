/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SPAKE2+ point arithmetic on ocrypto of nrf_oberon (NCS, ocrypto_spake2p_p256.h): constant time, no allocation, the
 * secret scalars stay arguments. The library is the one that nRF Security links for the Oberon PSA driver. */

#include <string.h>

#include "ocrypto_spake2p_p256.h"
#include "sensei_spake2p.h"

int sensei_spake2p_reduce(const uint8_t in[40], uint8_t out[32]) {
  ocrypto_spake2p_p256_reduce(out, in, 40);
  return 0;
}

/* The encoding is checked here (prefix 04: ocrypto reads x and y), the curve and the range by ocrypto. */
int sensei_spake2p_check(const uint8_t pt[65]) {
  return pt[0] == 0x04 && ocrypto_spake2p_p256_check_key(pt) == 0 ? 0 : -1;
}

int sensei_spake2p_share(const uint8_t w0[32], const uint8_t xy[32], const uint8_t mn[65], uint8_t share[65]) {
  if (sensei_spake2p_check(mn) != 0) {
    return -1;
  }
  return ocrypto_spake2p_p256_get_key_share(share, w0, xy, mn) == 0 ? 0 : -1;
}

int sensei_spake2p_zv(const uint8_t w0[32], const uint8_t *w1, const uint8_t xy[32], const uint8_t peer[65],
                      const uint8_t nm[65], const uint8_t *l, uint8_t z[65], uint8_t v[65]) {
  if ((w1 != NULL) == (l != NULL) || sensei_spake2p_check(peer) != 0 || sensei_spake2p_check(nm) != 0 ||
      (l && sensei_spake2p_check(l) != 0)) {
    return -1;
  }
  return ocrypto_spake2p_p256_get_ZV(z, v, w0, w1, xy, peer, nm, l) == 0 ? 0 : -1;
}
