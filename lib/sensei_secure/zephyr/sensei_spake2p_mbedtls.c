/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SPAKE2+ point arithmetic on the ECP module of Mbed TLS, for native_sim and BabbleSim only (CONFIG_SENSEI_SECURE_
 * SPAKE2P_MBEDTLS_ECP depends on the POSIX architecture). Not constant time (the ECP module of Mbed TLS is only
 * hardened against simple attacks), uses the heap of Mbed TLS, and needs the private ECP interface. The PSA API of the
 * NCS has no SPAKE2+ on these targets. Real boards use the ocrypto backend. */

#include <string.h>

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS /* before the first Mbed TLS header */
#include <psa/crypto.h>

#include <mbedtls/private/bignum.h>
#include <mbedtls/private/ecp.h>

#include "sensei_spake2p.h"

struct ctx {
  mbedtls_ecp_group grp;
};

/* mbedtls_ecp_mul() wants a random generator (it blinds the multiplication with it). */
static int rng(void *p, unsigned char *out, size_t n) {
  (void)p;
  return psa_generate_random(out, n) == PSA_SUCCESS ? 0 : -1;
}

static void ctx_free(struct ctx *c) { mbedtls_ecp_group_free(&c->grp); }

static int ctx_init(struct ctx *c) {
  int rc;

  mbedtls_ecp_group_init(&c->grp);
  rc = mbedtls_ecp_group_load(&c->grp, MBEDTLS_ECP_DP_SECP256R1);
  if (rc != 0) {
    ctx_free(c);
  }
  return rc;
}

/* A 65 B uncompressed point of the curve, not the identity. */
static int read_point(const struct ctx *c, mbedtls_ecp_point *p, const uint8_t b[65]) {
  return b[0] == 0x04 && mbedtls_ecp_point_read_binary(&c->grp, p, b, 65) == 0 &&
                 mbedtls_ecp_check_pubkey(&c->grp, p) == 0 && mbedtls_ecp_is_zero(p) == 0
             ? 0
             : -1;
}

static int write_point(const struct ctx *c, const mbedtls_ecp_point *p, uint8_t out[65]) {
  size_t n = 0;

  return mbedtls_ecp_is_zero((mbedtls_ecp_point *)p) == 0 &&
                 mbedtls_ecp_point_write_binary(&c->grp, p, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, out, 65) == 0 && n == 65
             ? 0
             : -1;
}

int sensei_spake2p_reduce(const uint8_t in[40], uint8_t out[32]) {
  struct ctx c;
  mbedtls_mpi a, r;
  int rc = -1;

  mbedtls_mpi_init(&a);
  mbedtls_mpi_init(&r);
  if (ctx_init(&c) == 0 && mbedtls_mpi_read_binary(&a, in, 40) == 0 && mbedtls_mpi_mod_mpi(&r, &a, &c.grp.N) == 0 &&
      mbedtls_mpi_write_binary(&r, out, 32) == 0) {
    rc = 0;
  }
  mbedtls_mpi_free(&a);
  mbedtls_mpi_free(&r);
  ctx_free(&c);
  return rc;
}

int sensei_spake2p_check(const uint8_t pt[65]) {
  struct ctx c;
  mbedtls_ecp_point p;
  int rc = -1;

  mbedtls_ecp_point_init(&p);
  if (ctx_init(&c) == 0) {
    rc = read_point(&c, &p, pt);
  }
  mbedtls_ecp_point_free(&p);
  ctx_free(&c);
  return rc;
}

int sensei_spake2p_share(const uint8_t w0[32], const uint8_t xy[32], const uint8_t mn[65], uint8_t share[65]) {
  struct ctx c;
  mbedtls_mpi bw, bx;
  mbedtls_ecp_point m, r;
  int rc = -1;

  mbedtls_mpi_init(&bw);
  mbedtls_mpi_init(&bx);
  mbedtls_ecp_point_init(&m);
  mbedtls_ecp_point_init(&r);
  if (ctx_init(&c) == 0 && read_point(&c, &m, mn) == 0 && mbedtls_mpi_read_binary(&bw, w0, 32) == 0 &&
      mbedtls_mpi_read_binary(&bx, xy, 32) == 0 &&
      mbedtls_ecp_muladd(&c.grp, &r, &bx, &c.grp.G, &bw, &m) == 0) { /* xy*G + w0*mn */
    rc = write_point(&c, &r, share);
  }
  mbedtls_mpi_free(&bw);
  mbedtls_mpi_free(&bx);
  mbedtls_ecp_point_free(&m);
  mbedtls_ecp_point_free(&r);
  ctx_free(&c);
  return rc;
}

int sensei_spake2p_zv(const uint8_t w0[32], const uint8_t *w1, const uint8_t xy[32], const uint8_t peer[65],
                      const uint8_t nm[65], const uint8_t *l, uint8_t z[65], uint8_t v[65]) {
  struct ctx c;
  mbedtls_mpi bw0, bw1, bx, one, neg;
  mbedtls_ecp_point pp, pn, pl, rest, rz, rv;
  int rc = -1;

  mbedtls_mpi_init(&bw0);
  mbedtls_mpi_init(&bw1);
  mbedtls_mpi_init(&bx);
  mbedtls_mpi_init(&one);
  mbedtls_mpi_init(&neg);
  mbedtls_ecp_point_init(&pp);
  mbedtls_ecp_point_init(&pn);
  mbedtls_ecp_point_init(&pl);
  mbedtls_ecp_point_init(&rest);
  mbedtls_ecp_point_init(&rz);
  mbedtls_ecp_point_init(&rv);
  if ((w1 != NULL) == (l != NULL) || ctx_init(&c) != 0) {
    goto out_noctx; /* no group to free: ctx_init() frees it when it fails */
  }
  if (read_point(&c, &pp, peer) != 0 || read_point(&c, &pn, nm) != 0 || (l && read_point(&c, &pl, l) != 0) ||
      mbedtls_mpi_read_binary(&bw0, w0, 32) != 0 || mbedtls_mpi_read_binary(&bx, xy, 32) != 0 ||
      (w1 && mbedtls_mpi_read_binary(&bw1, w1, 32) != 0) || mbedtls_mpi_lset(&one, 1) != 0) {
    goto out;
  }
  /* rest = peer - w0*nm = 1*peer + (n - w0)*nm */
  if (mbedtls_mpi_sub_mpi(&neg, &c.grp.N, &bw0) != 0 || mbedtls_mpi_mod_mpi(&neg, &neg, &c.grp.N) != 0 ||
      mbedtls_ecp_muladd(&c.grp, &rest, &one, &pp, &neg, &pn) != 0 || mbedtls_ecp_is_zero(&rest)) {
    goto out;
  }
  if (mbedtls_ecp_mul(&c.grp, &rz, &bx, &rest, rng, NULL) != 0 ||
      (w1 ? mbedtls_ecp_mul(&c.grp, &rv, &bw1, &rest, rng, NULL) : mbedtls_ecp_mul(&c.grp, &rv, &bx, &pl, rng, NULL)) !=
          0) {
    goto out;
  }
  if (write_point(&c, &rz, z) == 0 && write_point(&c, &rv, v) == 0) {
    rc = 0;
  }
out:
  ctx_free(&c);
out_noctx:
  mbedtls_mpi_free(&bw0);
  mbedtls_mpi_free(&bw1);
  mbedtls_mpi_free(&bx);
  mbedtls_mpi_free(&one);
  mbedtls_mpi_free(&neg);
  mbedtls_ecp_point_free(&pp);
  mbedtls_ecp_point_free(&pn);
  mbedtls_ecp_point_free(&pl);
  mbedtls_ecp_point_free(&rest);
  mbedtls_ecp_point_free(&rz);
  mbedtls_ecp_point_free(&rv);
  return rc;
}
