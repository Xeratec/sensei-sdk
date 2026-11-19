/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Sealed settings records (src/sensei_e2e_seal.c, keys at rest) with the OpenSSL backend, against the vectors of the
 * Python implementation (vector 7). */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "sensei_e2e_seal.h"
#include "vectors.h"

static const struct sensei_secure_crypto *C;

static sensei_secure_key_t import(const uint8_t *key) {
  sensei_secure_key_t k = 0;

  CHECK(C->aead_import(C->ctx, key, &k) == 0);
  return k;
}

static void test_vectors(void) {
  sensei_secure_key_t k = import(vec_seal_key);

  for (size_t i = 0; i < VEC_SEAL_COUNT; i++) {
    const vec_seal_t *v = &vec_seal[i];
    uint8_t rec[256], plain[256];

    /* the C side makes the same record the Python side made */
    int n = sensei_e2e_seal_record_ctx(C, k, v->name, v->ctx, v->ctx_len, v->nonce, v->plain, v->plain_len, rec,
                                       sizeof(rec));
    CHECK(n == (int)v->record_len);
    CHECK(n == (int)(v->plain_len + SENSEI_E2E_SEAL_OVERHEAD));
    CHECK(n > 0 && memcmp(rec, v->record, v->record_len) == 0);
    /* and opens it */
    n = sensei_e2e_open_record_ctx(C, k, v->name, v->ctx, v->ctx_len, v->record, v->record_len, plain, sizeof(plain));
    CHECK(n == (int)v->plain_len);
    CHECK(n >= 0 && memcmp(plain, v->plain, v->plain_len) == 0);
    if (v->ctx_len) { /* the context is part of the associated data: without it, or another one, it does not open */
      uint8_t other[SENSEI_E2E_SEAL_CTX_MAX];

      memcpy(other, v->ctx, v->ctx_len);
      CHECK(sensei_e2e_open_record(C, k, v->name, v->record, v->record_len, plain, sizeof(plain)) == -EACCES);
      other[0] ^= 1;
      CHECK(sensei_e2e_open_record_ctx(C, k, v->name, other, v->ctx_len, v->record, v->record_len, plain,
                                       sizeof(plain)) == -EACCES);
      CHECK(sensei_e2e_open_record_ctx(C, k, v->name, v->ctx, v->ctx_len - 1, v->record, v->record_len, plain,
                                       sizeof(plain)) == -EACCES);
      CHECK(sensei_e2e_seal_record_ctx(C, k, v->name, v->ctx, SENSEI_E2E_SEAL_CTX_MAX + 1, NULL, v->plain, v->plain_len,
                                       rec, sizeof(rec)) == -EINVAL);
    }
  }
  C->key_destroy(C->ctx, k);
}

static void test_negatives(void) {
  sensei_secure_key_t k = import(vec_seal_key);
  uint8_t other[16], plain[64], rec[64 + SENSEI_E2E_SEAL_OVERHEAD];
  const vec_seal_t *v = &vec_seal[0]; /* e2e_privx, 32 B */
  sensei_secure_key_t k2;

  memcpy(other, vec_seal_key, sizeof(other));
  other[0] ^= 1;
  k2 = import(other);
  /* wrong name: the record does not open in another place */
  CHECK(sensei_e2e_open_record(C, k, "e2e_rat", v->record, v->record_len, plain, sizeof(plain)) == -EACCES);
  CHECK(sensei_e2e_open_record(C, k, "e2e_priv", v->record, v->record_len, plain, sizeof(plain)) == -EACCES);
  /* wrong key */
  CHECK(sensei_e2e_open_record(C, k2, v->name, v->record, v->record_len, plain, sizeof(plain)) == -EACCES);
  /* every byte of the record is covered: version, nonce, ciphertext, tag */
  for (size_t i = 0; i < v->record_len; i++) {
    memcpy(rec, v->record, v->record_len);
    rec[i] ^= 0x01;
    CHECK(sensei_e2e_open_record(C, k, v->name, rec, v->record_len, plain, sizeof(plain)) ==
          (i == 0 ? -EBADMSG : -EACCES));
  }
  /* short, truncated, extended */
  CHECK(sensei_e2e_open_record(C, k, v->name, v->record, SENSEI_E2E_SEAL_OVERHEAD - 1, plain, sizeof(plain)) ==
        -EBADMSG);
  CHECK(sensei_e2e_open_record(C, k, v->name, v->record, v->record_len - 1, plain, sizeof(plain)) == -EACCES);
  memcpy(rec, v->record, v->record_len);
  rec[v->record_len] = 0;
  CHECK(sensei_e2e_open_record(C, k, v->name, rec, v->record_len + 1, plain, sizeof(plain)) == -EACCES);
  /* output too small */
  CHECK(sensei_e2e_open_record(C, k, v->name, v->record, v->record_len, plain, 31) == -ENOSPC);
  /* a failed tag leaves no plaintext behind */
  memset(plain, 0xEE, sizeof(plain));
  CHECK(sensei_e2e_open_record(C, k2, v->name, v->record, v->record_len, plain, sizeof(plain)) == -EACCES);
  CHECK(memcmp(plain, v->plain, v->plain_len) != 0);
  /* names: empty, too long */
  CHECK(sensei_e2e_seal_record(C, k, "", NULL, v->plain, 4, rec, sizeof(rec)) == -EINVAL);
  CHECK(sensei_e2e_seal_record(C, k, NULL, NULL, v->plain, 4, rec, sizeof(rec)) == -EINVAL);
  CHECK(sensei_e2e_seal_record(C, k, "0123456789012345678901234567890123456789", NULL, v->plain, 4, rec, sizeof(rec)) ==
        -EINVAL);
  /* sealing into a buffer that is too small */
  CHECK(sensei_e2e_seal_record(C, k, v->name, NULL, v->plain, 32, rec, 32 + SENSEI_E2E_SEAL_OVERHEAD - 1) == -ENOSPC);
  C->key_destroy(C->ctx, k);
  C->key_destroy(C->ctx, k2);
}

static void test_random_nonce(void) {
  sensei_secure_key_t k = import(vec_seal_key);
  uint8_t a[64], b[64], plain[32];
  int na = sensei_e2e_seal_record(C, k, "e2e_privx", NULL, vec_seal_plain_0, 32, a, sizeof(a));
  int nb = sensei_e2e_seal_record(C, k, "e2e_privx", NULL, vec_seal_plain_0, 32, b, sizeof(b));

  CHECK(na == 61 && nb == 61);
  CHECK(a[0] == 1);                       /* version */
  CHECK(memcmp(a + 1, b + 1, 12) != 0);   /* a new nonce each time */
  CHECK(memcmp(a + 13, b + 13, 32) != 0); /* so another ciphertext */
  CHECK(sensei_e2e_open_record(C, k, "e2e_privx", a, 61, plain, sizeof(plain)) == 32);
  CHECK(memcmp(plain, vec_seal_plain_0, 32) == 0);
  C->key_destroy(C->ctx, k);
}

int main(void) {
  C = sensei_secure_crypto_openssl();
  CHECK(C != NULL);
  test_vectors();
  test_negatives();
  test_random_nonce();
  return CHECK_DONE();
}
