/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Host backend on OpenSSL 3, for tests and tools. Allocates; the device backend does not. */

#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include "sensei_secure.h"

struct handle {
  EVP_PKEY *pkey;  /* ECDH key pair */
  uint8_t aes[16]; /* AES-GCM key */
  int is_pkey;
};

static int c_random(void *ctx, uint8_t *out, size_t len) {
  (void)ctx;
  return RAND_bytes(out, (int)len) == 1 ? 0 : -1;
}

static int c_ecdh_import(void *ctx, const uint8_t *priv, size_t len, sensei_secure_key_t *key) {
  EVP_PKEY_CTX *pc = NULL;
  EVP_PKEY *pkey = NULL;
  OSSL_PARAM_BLD *bld = NULL;
  OSSL_PARAM *params = NULL;
  BIGNUM *bn = NULL;
  EC_GROUP *grp = NULL;
  EC_POINT *pt = NULL;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE];
  struct handle *h;
  int rc = -1;

  (void)ctx;
  if (len != 32) {
    return -1;
  }
  /* The public point is computed here: fromdata does not derive it from the private scalar. */
  bn = BN_bin2bn(priv, (int)len, NULL);
  grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  pt = grp ? EC_POINT_new(grp) : NULL;
  bld = OSSL_PARAM_BLD_new();
  if (!bn || !grp || !pt || !bld || BN_is_zero(bn) || BN_cmp(bn, EC_GROUP_get0_order(grp)) >= 0 ||
      EC_POINT_mul(grp, pt, bn, NULL, NULL, NULL) != 1 ||
      EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED, pub, sizeof(pub), NULL) != sizeof(pub) ||
      !OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bn) ||
      !OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub))) {
    goto out;
  }
  params = OSSL_PARAM_BLD_to_param(bld);
  pc = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  if (!params || !pc || EVP_PKEY_fromdata_init(pc) != 1 ||
      EVP_PKEY_fromdata(pc, &pkey, EVP_PKEY_KEYPAIR, params) != 1) {
    goto out;
  }
  h = calloc(1, sizeof(*h));
  if (!h) {
    goto out;
  }
  h->pkey = pkey;
  h->is_pkey = 1;
  pkey = NULL;
  *key = (sensei_secure_key_t)h;
  rc = 0;
out:
  EVP_PKEY_free(pkey);
  EVP_PKEY_CTX_free(pc);
  OSSL_PARAM_free(params);
  OSSL_PARAM_BLD_free(bld);
  EC_POINT_free(pt);
  EC_GROUP_free(grp);
  BN_free(bn);
  return rc;
}

static int c_ecdh_public(void *ctx, sensei_secure_key_t key, uint8_t *pub) {
  struct handle *h = (struct handle *)key;
  size_t n = 0;

  (void)ctx;
  if (!h || !h->is_pkey ||
      EVP_PKEY_get_octet_string_param(h->pkey, OSSL_PKEY_PARAM_PUB_KEY, pub, SENSEI_SECURE_PUB_SIZE, &n) != 1) {
    return -1;
  }
  return n == SENSEI_SECURE_PUB_SIZE ? 0 : -1;
}

static int c_ecdh_shared(void *ctx, sensei_secure_key_t key, const uint8_t *peer_pub, uint8_t *secret) {
  struct handle *h = (struct handle *)key;
  EVP_PKEY_CTX *pc = NULL, *dc = NULL;
  EVP_PKEY *peer = NULL;
  OSSL_PARAM params[3];
  size_t n = 32;
  int rc = -1;

  (void)ctx;
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0);
  params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void *)peer_pub, SENSEI_SECURE_PUB_SIZE);
  params[2] = OSSL_PARAM_construct_end();
  pc = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  if (!h || !pc || EVP_PKEY_fromdata_init(pc) != 1 || EVP_PKEY_fromdata(pc, &peer, EVP_PKEY_PUBLIC_KEY, params) != 1) {
    goto out;
  }
  dc = EVP_PKEY_CTX_new(h->pkey, NULL);
  if (!dc || EVP_PKEY_derive_init(dc) != 1 || EVP_PKEY_derive_set_peer(dc, peer) != 1 ||
      EVP_PKEY_derive(dc, secret, &n) != 1 || n != 32) {
    goto out;
  }
  rc = 0;
out:
  EVP_PKEY_free(peer);
  EVP_PKEY_CTX_free(pc);
  EVP_PKEY_CTX_free(dc);
  return rc;
}

static int c_hkdf(void *ctx, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                  const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
  EVP_KDF_CTX *kc = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
  OSSL_PARAM p[6];
  int rc = -1;

  (void)ctx;
  p[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0);
  p[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)ikm, ikm_len);
  p[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, salt_len);
  p[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, info_len);
  p[4] = OSSL_PARAM_construct_end();
  if (kc && EVP_KDF_derive(kc, okm, okm_len, p) == 1) {
    rc = 0;
  }
  EVP_KDF_CTX_free(kc);
  EVP_KDF_free(kdf);
  return rc;
}

static int c_aead_import(void *ctx, const uint8_t *key, sensei_secure_key_t *handle) {
  struct handle *h = calloc(1, sizeof(*h));

  (void)ctx;
  if (!h) {
    return -1;
  }
  memcpy(h->aes, key, 16);
  *handle = (sensei_secure_key_t)h;
  return 0;
}

static int c_key_destroy(void *ctx, sensei_secure_key_t key) {
  struct handle *h = (struct handle *)key;

  (void)ctx;
  if (h) {
    EVP_PKEY_free(h->pkey);
    OPENSSL_cleanse(h, sizeof(*h));
    free(h);
  }
  return 0;
}

static int c_aead_encrypt(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  struct handle *h = (struct handle *)key;
  EVP_CIPHER_CTX *e = EVP_CIPHER_CTX_new();
  int n = 0, m = 0, rc = -1;

  (void)ctx;
  if (out_cap < in_len + 16 || !e) {
    goto out;
  }
  if (EVP_EncryptInit_ex(e, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1 ||
      EVP_CIPHER_CTX_ctrl(e, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
      EVP_EncryptInit_ex(e, NULL, NULL, h->aes, nonce) != 1 || EVP_EncryptUpdate(e, NULL, &n, aad, (int)aad_len) != 1) {
    goto out;
  }
  if (in_len && EVP_EncryptUpdate(e, out, &n, in, (int)in_len) != 1) {
    goto out;
  }
  if (EVP_EncryptFinal_ex(e, out + n, &m) != 1 || EVP_CIPHER_CTX_ctrl(e, EVP_CTRL_GCM_GET_TAG, 16, out + in_len) != 1) {
    goto out;
  }
  *out_len = in_len + 16;
  rc = 0;
out:
  EVP_CIPHER_CTX_free(e);
  return rc;
}

static int c_aead_decrypt(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                          const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  struct handle *h = (struct handle *)key;
  EVP_CIPHER_CTX *e = EVP_CIPHER_CTX_new();
  int n = 0, m = 0, rc = -1;
  size_t pl;

  (void)ctx;
  if (in_len < 16 || !e) {
    goto out;
  }
  pl = in_len - 16;
  if (out_cap < pl) {
    goto out;
  }
  if (EVP_DecryptInit_ex(e, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1 ||
      EVP_CIPHER_CTX_ctrl(e, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
      EVP_DecryptInit_ex(e, NULL, NULL, h->aes, nonce) != 1 || EVP_DecryptUpdate(e, NULL, &n, aad, (int)aad_len) != 1) {
    goto out;
  }
  if (pl && EVP_DecryptUpdate(e, out, &n, in, (int)pl) != 1) {
    goto out;
  }
  if (EVP_CIPHER_CTX_ctrl(e, EVP_CTRL_GCM_SET_TAG, 16, (void *)(in + pl)) != 1 ||
      EVP_DecryptFinal_ex(e, out + n, &m) != 1) {
    if (pl) {
      OPENSSL_cleanse(out, pl);
    }
    goto out;
  }
  *out_len = pl;
  rc = 0;
out:
  EVP_CIPHER_CTX_free(e);
  return rc;
}

static int c_hmac_sha256(void *ctx, const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32]) {
  unsigned int n = 0;

  (void)ctx;
  if (key_len == 0 || key_len > INT32_MAX) {
    return -1;
  }
  return HMAC(EVP_sha256(), key, (int)key_len, msg ? msg : (const uint8_t *)"", msg_len, out, &n) && n == 32 ? 0 : -1;
}

/* X25519(priv, peer). OpenSSL clamps the scalar, ignores the top bit of the peer and fails if the result is all zero.
 */
static int c_x25519(void *ctx, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
  EVP_PKEY *a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
  EVP_PKEY *b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
  EVP_PKEY_CTX *dc = a ? EVP_PKEY_CTX_new(a, NULL) : NULL;
  size_t n = 32;
  int rc = -1;

  (void)ctx;
  if (dc && b && EVP_PKEY_derive_init(dc) == 1 && EVP_PKEY_derive_set_peer(dc, b) == 1 &&
      EVP_PKEY_derive(dc, out, &n) == 1 && n == 32) {
    rc = 0;
  }
  EVP_PKEY_CTX_free(dc);
  EVP_PKEY_free(a);
  EVP_PKEY_free(b);
  return rc;
}

static int c_x25519_keygen(void *ctx, uint8_t priv[32], uint8_t pub[32]) {
  static const uint8_t base[32] = {9};
  int rc;

  if (c_random(ctx, priv, 32) != 0) {
    return -1;
  }
  rc = c_x25519(ctx, priv, base, pub);
  if (rc != 0) {
    OPENSSL_cleanse(priv, 32);
  }
  return rc;
}

/* ---- commissioning: SHA-256, scalar reduction and the SPAKE2+ point operations (not constant time: host only) ---- */

static int c_sha256(void *ctx, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
  unsigned int n = 0;

  (void)ctx;
  return EVP_Digest(msg ? msg : (const uint8_t *)"", msg_len, out, &n, EVP_sha256(), NULL) == 1 && n == 32 ? 0 : -1;
}

static int c_scalar_reduce(void *ctx, const uint8_t in[40], uint8_t out[32]) {
  BN_CTX *bc = BN_CTX_new();
  EC_GROUP *grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  BIGNUM *a = BN_secure_new(), *r = BN_secure_new();
  int rc = -1;

  (void)ctx;
  if (bc && grp && a && r && BN_bin2bn(in, 40, a) && BN_nnmod(r, a, EC_GROUP_get0_order(grp), bc) == 1 &&
      BN_bn2binpad(r, out, 32) == 32) {
    rc = 0;
  }
  BN_clear_free(a);
  BN_clear_free(r);
  EC_GROUP_free(grp);
  BN_CTX_free(bc);
  return rc;
}

/* A 65 B uncompressed point of the curve (prefix 04, coordinates below p, on the curve), else NULL. */
static EC_POINT *load_point(const EC_GROUP *grp, const uint8_t pt[65], BN_CTX *bc) {
  EC_POINT *p = EC_POINT_new(grp);

  if (pt[0] != 0x04 || !p || EC_POINT_oct2point(grp, p, pt, 65, bc) != 1 || EC_POINT_is_at_infinity(grp, p)) {
    EC_POINT_free(p);
    return NULL;
  }
  return p;
}

static int store_point(const EC_GROUP *grp, const EC_POINT *p, uint8_t out[65], BN_CTX *bc) {
  return !EC_POINT_is_at_infinity(grp, p) &&
                 EC_POINT_point2oct(grp, p, POINT_CONVERSION_UNCOMPRESSED, out, 65, bc) == 65
             ? 0
             : -1;
}

static int c_spake2p_check(void *ctx, const uint8_t pt[65]) {
  BN_CTX *bc = BN_CTX_new();
  EC_GROUP *grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  EC_POINT *p = bc && grp ? load_point(grp, pt, bc) : NULL;
  int rc = p ? 0 : -1;

  (void)ctx;
  EC_POINT_free(p);
  EC_GROUP_free(grp);
  BN_CTX_free(bc);
  return rc;
}

static int c_spake2p_share(void *ctx, const uint8_t w0[32], const uint8_t xy[32], const uint8_t mn[65],
                           uint8_t share[65]) {
  BN_CTX *bc = BN_CTX_new();
  EC_GROUP *grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  BIGNUM *bw = BN_secure_new(), *bx = BN_secure_new();
  EC_POINT *m = bc && grp ? load_point(grp, mn, bc) : NULL;
  EC_POINT *r = grp ? EC_POINT_new(grp) : NULL;
  int rc = -1;

  (void)ctx;
  if (m && r && bw && bx && BN_bin2bn(w0, 32, bw) && BN_bin2bn(xy, 32, bx) &&
      EC_POINT_mul(grp, r, bx, m, bw, bc) == 1) { /* xy*G + w0*mn */
    rc = store_point(grp, r, share, bc);
  }
  BN_clear_free(bw);
  BN_clear_free(bx);
  EC_POINT_free(m);
  EC_POINT_clear_free(r);
  EC_GROUP_free(grp);
  BN_CTX_free(bc);
  return rc;
}

static int c_spake2p_zv(void *ctx, const uint8_t w0[32], const uint8_t *w1, const uint8_t xy[32],
                        const uint8_t peer[65], const uint8_t nm[65], const uint8_t *l, uint8_t z[65], uint8_t v[65]) {
  BN_CTX *bc = BN_CTX_new();
  EC_GROUP *grp = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  BIGNUM *bw0 = BN_secure_new(), *bw1 = BN_secure_new(), *bx = BN_secure_new();
  EC_POINT *pp = bc && grp ? load_point(grp, peer, bc) : NULL;
  EC_POINT *pn = bc && grp ? load_point(grp, nm, bc) : NULL;
  EC_POINT *pl = (bc && grp && l) ? load_point(grp, l, bc) : NULL;
  EC_POINT *t = grp ? EC_POINT_new(grp) : NULL, *rest = grp ? EC_POINT_new(grp) : NULL;
  EC_POINT *rz = grp ? EC_POINT_new(grp) : NULL, *rv = grp ? EC_POINT_new(grp) : NULL;
  int rc = -1;

  (void)ctx;
  if (!pp || !pn || !t || !rest || !rz || !rv || !bw0 || !bw1 || !bx || (w1 != NULL) == (l != NULL) || (l && !pl) ||
      !BN_bin2bn(w0, 32, bw0) || !BN_bin2bn(xy, 32, bx) || (w1 && !BN_bin2bn(w1, 32, bw1))) {
    goto out;
  }
  /* rest = peer - w0*nm */
  if (EC_POINT_mul(grp, t, NULL, pn, bw0, bc) != 1 || EC_POINT_invert(grp, t, bc) != 1 ||
      EC_POINT_add(grp, rest, pp, t, bc) != 1 || EC_POINT_is_at_infinity(grp, rest)) {
    goto out;
  }
  if (EC_POINT_mul(grp, rz, NULL, rest, bx, bc) != 1 ||
      (w1 ? EC_POINT_mul(grp, rv, NULL, rest, bw1, bc) : EC_POINT_mul(grp, rv, NULL, pl, bx, bc)) != 1) {
    goto out;
  }
  if (store_point(grp, rz, z, bc) == 0 && store_point(grp, rv, v, bc) == 0) {
    rc = 0;
  }
out:
  BN_clear_free(bw0);
  BN_clear_free(bw1);
  BN_clear_free(bx);
  EC_POINT_free(pp);
  EC_POINT_free(pn);
  EC_POINT_free(pl);
  EC_POINT_clear_free(t);
  EC_POINT_clear_free(rest);
  EC_POINT_clear_free(rz);
  EC_POINT_clear_free(rv);
  EC_GROUP_free(grp);
  BN_CTX_free(bc);
  return rc;
}

const struct sensei_secure_crypto *sensei_secure_crypto_openssl(void) {
  static const struct sensei_secure_crypto c = {
      .ctx = NULL,
      .random = c_random,
      .ecdh_import = c_ecdh_import,
      .ecdh_public = c_ecdh_public,
      .ecdh_shared = c_ecdh_shared,
      .hkdf = c_hkdf,
      .aead_import = c_aead_import,
      .key_destroy = c_key_destroy,
      .aead_encrypt = c_aead_encrypt,
      .aead_decrypt = c_aead_decrypt,
      .hmac_sha256 = c_hmac_sha256,
      .x25519_keygen = c_x25519_keygen,
      .x25519 = c_x25519,
      .sha256 = c_sha256,
      .scalar_reduce = c_scalar_reduce,
      .spake2p_check = c_spake2p_check,
      .spake2p_share = c_spake2p_share,
      .spake2p_zv = c_spake2p_zv,
  };

  return &c;
}
