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
  };

  return &c;
}
