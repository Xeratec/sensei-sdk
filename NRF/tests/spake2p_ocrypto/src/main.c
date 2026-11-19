/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The point arithmetic of the commissioning on ocrypto (nrf_oberon), against the vectors of the Python reference: the
 * RFC 9383 appendix C exchange (V9), the two sessions (V11), the shares that must be refused (V9 invalid points, V15b).
 * Runs on a Cortex-M33 in QEMU: the library is the one of the nRF5340. */

#include <string.h>

#include <zephyr/ztest.h>

#include "sensei_spake2p.h"
#include "vectors_commission.h"

ZTEST(spake2p_ocrypto, test_reduce) {
  uint8_t out[32];

  for (int i = 0; i < VEC_COM_RED_COUNT; i++) {
    zassert_equal(sensei_spake2p_reduce(vec_com_red[i].in, out), 0);
    zassert_mem_equal(out, vec_com_red[i].out, 32, "input %d", i);
  }
}

ZTEST(spake2p_ocrypto, test_check) {
  zassert_equal(sensei_spake2p_check(vec_com_m), 0);
  zassert_equal(sensei_spake2p_check(vec_com_n), 0);
  for (int i = 0; i < VEC_COM_MUL_COUNT; i++) {
    zassert_equal(sensei_spake2p_check(vec_com_mul[i].point), 0, "point %d", i);
  }
  for (int i = 0; i < VEC_COM_BAD_COUNT; i++) {
    if (vec_com_bad[i].len == 65) {
      zassert_not_equal(sensei_spake2p_check(vec_com_bad[i].data), 0, "%s", vec_com_bad[i].what);
    }
  }
}

ZTEST(spake2p_ocrypto, test_rfc9383_exchange) {
  uint8_t a[65], b[65], z[65], v[65];

  zassert_equal(sensei_spake2p_share(vec_com_rfc_w0, vec_com_rfc_x, vec_com_m, a), 0);
  zassert_mem_equal(a, vec_com_rfc_share_p, 65);
  zassert_equal(sensei_spake2p_share(vec_com_rfc_w0, vec_com_rfc_y, vec_com_n, b), 0);
  zassert_mem_equal(b, vec_com_rfc_share_v, 65);
  /* prover */
  zassert_equal(
      sensei_spake2p_zv(vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_x, vec_com_rfc_share_v, vec_com_n, NULL, z, v), 0);
  zassert_mem_equal(z, vec_com_rfc_z, 65);
  zassert_mem_equal(v, vec_com_rfc_v, 65);
  /* verifier */
  memset(z, 0, 65);
  memset(v, 0, 65);
  zassert_equal(
      sensei_spake2p_zv(vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, vec_com_m, vec_com_rfc_l, z, v), 0);
  zassert_mem_equal(z, vec_com_rfc_z, 65);
  zassert_mem_equal(v, vec_com_rfc_v, 65);
  /* both or neither of w1 and L */
  zassert_not_equal(sensei_spake2p_zv(vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_y, vec_com_rfc_share_p, vec_com_m,
                                      vec_com_rfc_l, z, v),
                    0);
  zassert_not_equal(sensei_spake2p_zv(vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, vec_com_m, NULL, z, v),
                    0);
}

ZTEST(spake2p_ocrypto, test_sessions) {
  for (int i = 0; i < VEC_COM_SES_COUNT; i++) {
    const vec_com_ses_t *s = &vec_com_ses[i];
    const vec_com_reg_t *r = &vec_com_reg[s->reg];
    uint8_t a[65], z[65], v[65];

    zassert_equal(sensei_spake2p_share(r->w0, s->x, vec_com_m, a), 0);
    zassert_mem_equal(a, s->share_p, 65);
    zassert_equal(sensei_spake2p_share(r->w0, s->y, vec_com_n, a), 0);
    zassert_mem_equal(a, s->share_v, 65);
    zassert_equal(sensei_spake2p_zv(r->w0, r->w1, s->x, s->share_v, vec_com_n, NULL, z, v), 0);
    zassert_mem_equal(z, s->z, 65);
    zassert_mem_equal(v, s->v, 65);
    zassert_equal(sensei_spake2p_zv(r->w0, NULL, s->y, s->share_p, vec_com_m, r->l, z, v), 0);
    zassert_mem_equal(z, s->z, 65);
    zassert_mem_equal(v, s->v, 65);
  }
}

ZTEST(spake2p_ocrypto, test_refused_shares) {
  uint8_t z[65], v[65], bad[65];

  /* the share w0*M (V15b): rest is the identity */
  for (int i = 0; i < VEC_COM_W0M_COUNT; i++) {
    const vec_com_ses_t *s = &vec_com_ses[vec_com_w0m[i].ses];
    const vec_com_reg_t *r = &vec_com_reg[s->reg];

    zassert_not_equal(sensei_spake2p_zv(r->w0, NULL, s->y, vec_com_w0m[i].share_p, vec_com_m, r->l, z, v), 0, "w0*M %d",
                      i);
  }
  /* the points that are no points, as the peer of both roles and as M or N */
  for (int i = 0; i < VEC_COM_BAD_COUNT; i++) {
    if (vec_com_bad[i].len != 65) {
      continue;
    }
    memcpy(bad, vec_com_bad[i].data, 65);
    zassert_not_equal(sensei_spake2p_zv(vec_com_rfc_w0, NULL, vec_com_rfc_y, bad, vec_com_m, vec_com_rfc_l, z, v), 0,
                      "%s", vec_com_bad[i].what);
    zassert_not_equal(sensei_spake2p_zv(vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_x, bad, vec_com_n, NULL, z, v), 0,
                      "%s", vec_com_bad[i].what);
    zassert_not_equal(sensei_spake2p_share(vec_com_rfc_w0, vec_com_rfc_x, bad, z), 0, "%s", vec_com_bad[i].what);
  }
}

ZTEST_SUITE(spake2p_ocrypto, NULL, NULL, NULL, NULL, NULL);
