/*
 * ----------------------------------------------------------------------
 *
 * File: tests.c
 *
 * Copyright (C) 2025 ETH Zurich and University of Bologna
 *
 * Based on the semester project of Ahmed Malanovic (ETH Zurich, 2025).
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Test suite of the SENSEI crypto library.
 *
 * Known-answer tests (KAT) use published vectors:
 *   SHA-256       FIPS 180-2 examples ("abc", 448-bit message, empty message)
 *   HMAC-SHA256   RFC 4231 test cases 1 and 2
 *   HKDF-SHA256   RFC 5869 test cases 1 and 3
 *   AES-GCM       McGrew/Viega GCM spec test cases 2 (zero block), 4 (AES-128 with AAD),
 *                 13 (empty), 14, 16 (AES-256 with AAD), as used by NIST SP 800-38D
 *   ECDSA P-256   RFC 6979 A.2.5 (public key derivation and signature verification)
 *   ECDH P-256    RFC 5903 section 8.1 (public keys and shared secret)
 * The remaining tests are round trips and negative tests.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "crypto/sensei_crypto.h"
#include "example.h"

static int n_checks;
static int n_failed;

static void check(const char *name, bool ok) {
  n_checks++;
  if (!ok) {
    n_failed++;
  }
  printk("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}

static bool status_is(sensei_crypto_status_t got, sensei_crypto_status_t want) {
  if (got != want) {
    printk("      status %d, expected %d\n", (int)got, (int)want);
  }
  return got == want;
}

/** Convert a hex string to bytes, returns the length. */
static size_t unhex(const char *hex, uint8_t *out, size_t out_size) {
  size_t n = strlen(hex) / 2;
  __ASSERT_NO_MSG(n <= out_size);
  if (n > 0) {
    hex2bin(hex, strlen(hex), out, out_size);
  }
  return n;
}

static bool equals_hex(const uint8_t *data, size_t len, const char *hex) {
  uint8_t exp[128];
  size_t n = unhex(hex, exp, sizeof(exp));
  return n == len && memcmp(exp, data, len) == 0;
}

/* ---------------------------------------------------------------- SHA-256 */

static void test_sha256(void) {
  static const struct {
    const char *name;
    const char *msg;
    const char *digest;
  } v[] = {
      {"SHA-256 KAT empty", "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"SHA-256 KAT abc", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"SHA-256 KAT 448-bit", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
  };
  uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE];

  for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
    sensei_crypto_status_t st = sensei_crypto_sha256((const uint8_t *)v[i].msg, strlen(v[i].msg), digest);
    check(v[i].name, status_is(st, SENSEI_CRYPTO_OK) && equals_hex(digest, sizeof(digest), v[i].digest));
  }

  check("SHA-256 NULL message with length",
        status_is(sensei_crypto_sha256(NULL, 1, digest), SENSEI_CRYPTO_ERR_INVALID_ARG));
}

/* ------------------------------------------------------------------- HMAC */

static void test_hmac(void) {
  uint8_t key[32], msg[64], mac[SENSEI_CRYPTO_SHA256_SIZE];
  size_t klen, mlen;
  sensei_crypto_status_t st;

  klen = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", key, sizeof(key));
  mlen = unhex("4869205468657265", msg, sizeof(msg)); /* "Hi There" */
  st = sensei_crypto_hmac_sha256(key, klen, msg, mlen, mac);
  check("HMAC-SHA256 KAT RFC 4231 #1",
        status_is(st, SENSEI_CRYPTO_OK) &&
            equals_hex(mac, sizeof(mac), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

  const char *jefe = "Jefe";
  const char *what = "what do ya want for nothing?";
  st = sensei_crypto_hmac_sha256((const uint8_t *)jefe, strlen(jefe), (const uint8_t *)what, strlen(what), mac);
  check("HMAC-SHA256 KAT RFC 4231 #2",
        status_is(st, SENSEI_CRYPTO_OK) &&
            equals_hex(mac, sizeof(mac), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

/* ------------------------------------------------------------------- HKDF */

static void test_hkdf(void) {
  uint8_t ikm[22], salt[13], info[10], okm[42];
  sensei_crypto_status_t st;

  memset(ikm, 0x0b, sizeof(ikm));
  for (size_t i = 0; i < sizeof(salt); i++) {
    salt[i] = (uint8_t)i;
  }
  for (size_t i = 0; i < sizeof(info); i++) {
    info[i] = (uint8_t)(0xf0 + i);
  }

  st = sensei_crypto_hkdf_sha256(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info), okm, sizeof(okm));
  check("HKDF-SHA256 KAT RFC 5869 #1",
        status_is(st, SENSEI_CRYPTO_OK) && equals_hex(okm, sizeof(okm),
                                                      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                                      "34007208d5b887185865"));

  /* Test case 3: no salt, no info. */
  st = sensei_crypto_hkdf_sha256(ikm, sizeof(ikm), NULL, 0, NULL, 0, okm, sizeof(okm));
  check("HKDF-SHA256 KAT RFC 5869 #3 (no salt, no info)",
        status_is(st, SENSEI_CRYPTO_OK) && equals_hex(okm, sizeof(okm),
                                                      "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                                                      "9d201395faa4b61a96c8"));

  st = sensei_crypto_hkdf_sha256(ikm, sizeof(ikm), NULL, 0, NULL, 0, okm, SENSEI_CRYPTO_HKDF_MAX_OUTPUT_SIZE + 1);
  check("HKDF-SHA256 output too long rejected", status_is(st, SENSEI_CRYPTO_ERR_INVALID_ARG));
}

/* ---------------------------------------------------------------- AES-GCM */

struct gcm_vec {
  const char *name;
  const char *key, *nonce, *aad, *pt, *ct, *tag;
};

static const struct gcm_vec gcm_vecs[] = {
    {"GCM-256 KAT #13 empty", "0000000000000000000000000000000000000000000000000000000000000000",
     "000000000000000000000000", "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
    {"GCM-256 KAT #14 zero block", "0000000000000000000000000000000000000000000000000000000000000000",
     "000000000000000000000000", "", "00000000000000000000000000000000", "cea7403d4d606b6e074ec5d3baf39d18",
     "d0d1c8a799996bf0265b98b5d48ab919"},
    {"GCM-128 KAT #4 with AAD", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
     "feedfacedeadbeeffeedfacedeadbeefabaddad2",
     "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
     "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
     "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
     "5bc94fbc3221a5db94fae95ae7121a47"},
    {"GCM-256 KAT #16 with AAD", "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
     "cafebabefacedbaddecaf888", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
     "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
     "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
     "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
     "76fc6ece0f4e1768cddf8853bb2d551b"},
};

static void test_gcm_kat(const struct gcm_vec *v) {
  uint8_t key[32], nonce[12], aad[32], pt[64], exp[96], out[96], dec[64];
  size_t klen = unhex(v->key, key, sizeof(key));
  size_t alen = unhex(v->aad, aad, sizeof(aad));
  size_t plen = unhex(v->pt, pt, sizeof(pt));
  size_t elen;
  size_t olen = 0, dlen = 0;
  sensei_crypto_key_id_t id = 0;
  char name[96];

  unhex(v->nonce, nonce, sizeof(nonce));
  elen = unhex(v->ct, exp, sizeof(exp));
  elen += unhex(v->tag, exp + elen, sizeof(exp) - elen);

  sensei_crypto_status_t st = sensei_crypto_aead_import_key(key, klen, &id);
  if (st != SENSEI_CRYPTO_OK) {
    check(v->name, status_is(st, SENSEI_CRYPTO_OK));
    return;
  }

  st = sensei_crypto_aead_encrypt(id, nonce, aad, alen, pt, plen, out, sizeof(out), &olen);
  snprintk(name, sizeof(name), "%s encrypt", v->name);
  check(name, status_is(st, SENSEI_CRYPTO_OK) && olen == elen && memcmp(out, exp, elen) == 0);

  st = sensei_crypto_aead_decrypt(id, nonce, aad, alen, exp, elen, dec, sizeof(dec), &dlen);
  snprintk(name, sizeof(name), "%s decrypt", v->name);
  check(name, status_is(st, SENSEI_CRYPTO_OK) && dlen == plen && memcmp(dec, pt, plen) == 0);

  sensei_crypto_destroy_key(id);
}

static void test_aead(void) {
  for (size_t i = 0; i < ARRAY_SIZE(gcm_vecs); i++) {
    test_gcm_kat(&gcm_vecs[i]);
  }

  /* Round trip, negative tests and argument checks with a generated key. */
  sensei_crypto_key_id_t id = 0;
  uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE];
  uint8_t aad[] = "header";
  uint8_t pt[100], ct[100 + SENSEI_CRYPTO_AES_GCM_TAG_SIZE], dec[100];
  size_t clen = 0, dlen = 0;

  for (size_t i = 0; i < sizeof(pt); i++) {
    pt[i] = (uint8_t)i;
  }

  check("AES-GCM generate 256-bit key", status_is(sensei_crypto_aead_generate_key(256, &id), SENSEI_CRYPTO_OK));
  check("AES-GCM generate nonce", status_is(sensei_crypto_aead_generate_nonce(nonce), SENSEI_CRYPTO_OK));

  sensei_crypto_status_t st =
      sensei_crypto_aead_encrypt(id, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, sizeof(ct), &clen);
  check("AES-GCM round trip encrypt",
        status_is(st, SENSEI_CRYPTO_OK) && clen == sizeof(pt) + SENSEI_CRYPTO_AES_GCM_TAG_SIZE);

  st = sensei_crypto_aead_decrypt(id, nonce, aad, sizeof(aad), ct, clen, dec, sizeof(dec), &dlen);
  check("AES-GCM round trip decrypt",
        status_is(st, SENSEI_CRYPTO_OK) && dlen == sizeof(pt) && memcmp(dec, pt, dlen) == 0);

  ct[5] ^= 0x01;
  st = sensei_crypto_aead_decrypt(id, nonce, aad, sizeof(aad), ct, clen, dec, sizeof(dec), &dlen);
  check("AES-GCM tampered ciphertext rejected", status_is(st, SENSEI_CRYPTO_ERR_AUTH_FAILED));
  ct[5] ^= 0x01;

  st = sensei_crypto_aead_decrypt(id, nonce, (const uint8_t *)"HEADER", sizeof(aad), ct, clen, dec, sizeof(dec), &dlen);
  check("AES-GCM wrong AAD rejected", status_is(st, SENSEI_CRYPTO_ERR_AUTH_FAILED));

  st = sensei_crypto_aead_encrypt(id, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, sizeof(pt), &clen);
  check("AES-GCM encrypt output too small", status_is(st, SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL));

  st = sensei_crypto_aead_decrypt(id, nonce, aad, sizeof(aad), ct, 4, dec, sizeof(dec), &dlen);
  check("AES-GCM ciphertext shorter than tag rejected", status_is(st, SENSEI_CRYPTO_ERR_INVALID_ARG));

  uint8_t k7[7] = {0};
  sensei_crypto_key_id_t bad = 0;
  check("AES-GCM bad key length rejected",
        status_is(sensei_crypto_aead_import_key(k7, sizeof(k7), &bad), SENSEI_CRYPTO_ERR_INVALID_ARG));

  sensei_crypto_destroy_key(id);
  st = sensei_crypto_aead_encrypt(id, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, sizeof(ct), &clen);
  check("AES-GCM destroyed key rejected", st != SENSEI_CRYPTO_OK);
}

/* ------------------------------------------------------------------ ECDSA */

/* RFC 6979 A.2.5, P-256 / SHA-256, message "sample" */
#define RFC6979_PRIV "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"
#define RFC6979_PUB                                                                                                    \
  "0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"                                                 \
  "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"
#define RFC6979_SIG                                                                                                    \
  "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"                                                   \
  "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"

static void test_ecdsa(void) {
  uint8_t priv[32], pub[65], sig[64], out[SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE];
  uint8_t exported[SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE];
  const uint8_t msg[] = "sample";
  size_t exp_len = 0, sig_len = 0;
  sensei_crypto_key_id_t priv_id = 0, pub_id = 0, gen_id = 0;
  sensei_crypto_status_t st;

  unhex(RFC6979_PRIV, priv, sizeof(priv));
  unhex(RFC6979_PUB, pub, sizeof(pub));
  unhex(RFC6979_SIG, sig, sizeof(sig));

  st = sensei_crypto_ecdsa_import_private_key(priv, sizeof(priv), &priv_id);
  check("ECDSA import private key", status_is(st, SENSEI_CRYPTO_OK));

  st = sensei_crypto_ecdsa_export_public_key(priv_id, exported, sizeof(exported), &exp_len);
  check("ECDSA KAT public key from private key (RFC 6979)",
        status_is(st, SENSEI_CRYPTO_OK) && exp_len == sizeof(pub) && memcmp(exported, pub, sizeof(pub)) == 0);

  st = sensei_crypto_ecdsa_import_public_key(pub, sizeof(pub), &pub_id);
  check("ECDSA import public key", status_is(st, SENSEI_CRYPTO_OK));

  check("ECDSA KAT verify RFC 6979 signature",
        status_is(sensei_crypto_ecdsa_verify(pub_id, msg, sizeof(msg) - 1, sig, sizeof(sig)), SENSEI_CRYPTO_OK));

  sig[10] ^= 0x01;
  check("ECDSA tampered signature rejected",
        status_is(sensei_crypto_ecdsa_verify(pub_id, msg, sizeof(msg) - 1, sig, sizeof(sig)),
                  SENSEI_CRYPTO_ERR_AUTH_FAILED));
  sig[10] ^= 0x01;

  check("ECDSA wrong message rejected",
        status_is(sensei_crypto_ecdsa_verify(pub_id, (const uint8_t *)"Sample", 6, sig, sizeof(sig)),
                  SENSEI_CRYPTO_ERR_AUTH_FAILED));

  /* Sign with the imported private key, verify with the imported public key. */
  st = sensei_crypto_ecdsa_sign(priv_id, msg, sizeof(msg) - 1, out, sizeof(out), &sig_len);
  check("ECDSA sign (imported key)", status_is(st, SENSEI_CRYPTO_OK) && sig_len == SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE);
  check("ECDSA verify own signature (imported public key)",
        status_is(sensei_crypto_ecdsa_verify(pub_id, msg, sizeof(msg) - 1, out, sig_len), SENSEI_CRYPTO_OK));

  /* Generated key round trip using the hash interface. */
  uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE];
  check("ECDSA generate key pair", status_is(sensei_crypto_ecdsa_generate_keypair(&gen_id), SENSEI_CRYPTO_OK));
  sensei_crypto_sha256(msg, sizeof(msg) - 1, digest);
  st = sensei_crypto_ecdsa_sign_hash(gen_id, digest, sizeof(digest), out, sizeof(out), &sig_len);
  check("ECDSA sign hash (generated key)", status_is(st, SENSEI_CRYPTO_OK));
  check("ECDSA verify hash (generated key)",
        status_is(sensei_crypto_ecdsa_verify_hash(gen_id, digest, sizeof(digest), out, sig_len), SENSEI_CRYPTO_OK));
  check("ECDSA signature of other key rejected",
        status_is(sensei_crypto_ecdsa_verify_hash(pub_id, digest, sizeof(digest), out, sig_len),
                  SENSEI_CRYPTO_ERR_AUTH_FAILED));
  check("ECDSA signature buffer too small",
        status_is(sensei_crypto_ecdsa_sign_hash(gen_id, digest, sizeof(digest), out, 32, &sig_len),
                  SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL));

  sensei_crypto_destroy_key(priv_id);
  sensei_crypto_destroy_key(pub_id);
  sensei_crypto_destroy_key(gen_id);
}

/* ------------------------------------------------------------------- ECDH */

/* RFC 5903 section 8.1 (256-bit random ECP group) */
#define RFC5903_I_PRIV "c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433"
#define RFC5903_I_PUB                                                                                                  \
  "04dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c3772581180"                                                 \
  "5271a0461cdb8252d61f1c456fa3e59ab1f45b33accf5f58389e0577b8990bb3"
#define RFC5903_R_PRIV "c6ef9c5d78ae012a011164acb397ce2088685d8f06bf9be0b283ab46476bee53"
#define RFC5903_R_PUB                                                                                                  \
  "04d12dfb5289c8d4f81208b70270398c342296970a0bccb74c736fc7554494bf63"                                                 \
  "56fbf3ca366cc23e8157854c13c58d6aac23f046ada30f8353e74f33039872ab"
#define RFC5903_SECRET "d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de"

static void test_ecdh(void) {
  uint8_t ipriv[32], rpriv[32], ipub[65], rpub[65], exported[65];
  uint8_t s1[SENSEI_CRYPTO_ECDH_SECRET_SIZE], s2[SENSEI_CRYPTO_ECDH_SECRET_SIZE];
  size_t elen = 0, l1 = 0, l2 = 0;
  sensei_crypto_key_id_t i_id = 0, r_id = 0;
  sensei_crypto_status_t st;

  unhex(RFC5903_I_PRIV, ipriv, sizeof(ipriv));
  unhex(RFC5903_R_PRIV, rpriv, sizeof(rpriv));
  unhex(RFC5903_I_PUB, ipub, sizeof(ipub));
  unhex(RFC5903_R_PUB, rpub, sizeof(rpub));

  check("ECDH import initiator key",
        status_is(sensei_crypto_ecdh_import_private_key(ipriv, sizeof(ipriv), &i_id), SENSEI_CRYPTO_OK));
  check("ECDH import responder key",
        status_is(sensei_crypto_ecdh_import_private_key(rpriv, sizeof(rpriv), &r_id), SENSEI_CRYPTO_OK));

  st = sensei_crypto_ecdh_export_public_key(i_id, exported, sizeof(exported), &elen);
  check("ECDH KAT initiator public key (RFC 5903)",
        status_is(st, SENSEI_CRYPTO_OK) && elen == sizeof(ipub) && memcmp(exported, ipub, sizeof(ipub)) == 0);
  st = sensei_crypto_ecdh_export_public_key(r_id, exported, sizeof(exported), &elen);
  check("ECDH KAT responder public key (RFC 5903)",
        status_is(st, SENSEI_CRYPTO_OK) && elen == sizeof(rpub) && memcmp(exported, rpub, sizeof(rpub)) == 0);

  st = sensei_crypto_ecdh_shared_secret(i_id, rpub, sizeof(rpub), s1, sizeof(s1), &l1);
  check("ECDH KAT shared secret, initiator side (RFC 5903)",
        status_is(st, SENSEI_CRYPTO_OK) && equals_hex(s1, l1, RFC5903_SECRET));
  st = sensei_crypto_ecdh_shared_secret(r_id, ipub, sizeof(ipub), s2, sizeof(s2), &l2);
  check("ECDH KAT shared secret, responder side (RFC 5903)",
        status_is(st, SENSEI_CRYPTO_OK) && equals_hex(s2, l2, RFC5903_SECRET));

  sensei_crypto_destroy_key(i_id);
  sensei_crypto_destroy_key(r_id);

  /* Two freshly generated key pairs must agree, and HKDF turns the secret into an AES key. */
  sensei_crypto_key_id_t a_id = 0, b_id = 0;
  uint8_t apub[65], bpub[65], sa[32], sb[32];
  size_t alen = 0, blen = 0, sal = 0, sbl = 0;

  check("ECDH generate key pairs", status_is(sensei_crypto_ecdh_generate_keypair(&a_id), SENSEI_CRYPTO_OK) &&
                                       status_is(sensei_crypto_ecdh_generate_keypair(&b_id), SENSEI_CRYPTO_OK));
  sensei_crypto_ecdh_export_public_key(a_id, apub, sizeof(apub), &alen);
  sensei_crypto_ecdh_export_public_key(b_id, bpub, sizeof(bpub), &blen);
  sensei_crypto_ecdh_shared_secret(a_id, bpub, blen, sa, sizeof(sa), &sal);
  sensei_crypto_ecdh_shared_secret(b_id, apub, alen, sb, sizeof(sb), &sbl);
  check("ECDH both sides agree (generated keys)", sal == sizeof(sa) && sbl == sizeof(sb) && memcmp(sa, sb, sal) == 0);

  uint8_t bad_pub[65];
  memcpy(bad_pub, apub, sizeof(bad_pub));
  bad_pub[64] ^= 0x01; /* not on the curve */
  st = sensei_crypto_ecdh_shared_secret(b_id, bad_pub, sizeof(bad_pub), sb, sizeof(sb), &sbl);
  check("ECDH point not on curve rejected", st != SENSEI_CRYPTO_OK);

  st = sensei_crypto_ecdh_shared_secret(b_id, apub, alen, sb, 16, &sbl);
  check("ECDH secret buffer too small", status_is(st, SENSEI_CRYPTO_ERR_BUFFER_TOO_SMALL));

  sensei_crypto_destroy_key(a_id);
  sensei_crypto_destroy_key(b_id);
}

/* A short end-to-end flow: ECDH -> HKDF -> AES-GCM between two parties. */
static void test_key_agreement_flow(void) {
  sensei_crypto_key_id_t a_id = 0, b_id = 0, a_aes = 0, b_aes = 0;
  uint8_t apub[65], bpub[65], sa[32], sb[32], ka[32], kb[32];
  uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE];
  uint8_t ct[32 + SENSEI_CRYPTO_AES_GCM_TAG_SIZE], pt[32];
  const uint8_t msg[] = "sensor reading 23.5 C";
  const uint8_t info[] = "sensei-crypto example";
  size_t alen = 0, blen = 0, sal = 0, sbl = 0, clen = 0, plen = 0;
  bool ok = true;

  ok &= sensei_crypto_ecdh_generate_keypair(&a_id) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_ecdh_generate_keypair(&b_id) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_ecdh_export_public_key(a_id, apub, sizeof(apub), &alen) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_ecdh_export_public_key(b_id, bpub, sizeof(bpub), &blen) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_ecdh_shared_secret(a_id, bpub, blen, sa, sizeof(sa), &sal) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_ecdh_shared_secret(b_id, apub, alen, sb, sizeof(sb), &sbl) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_hkdf_sha256(sa, sal, NULL, 0, info, sizeof(info) - 1, ka, sizeof(ka)) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_hkdf_sha256(sb, sbl, NULL, 0, info, sizeof(info) - 1, kb, sizeof(kb)) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_aead_import_key(ka, sizeof(ka), &a_aes) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_aead_import_key(kb, sizeof(kb), &b_aes) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_aead_generate_nonce(nonce) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_aead_encrypt(a_aes, nonce, NULL, 0, msg, sizeof(msg), ct, sizeof(ct), &clen) == SENSEI_CRYPTO_OK;
  ok &= sensei_crypto_aead_decrypt(b_aes, nonce, NULL, 0, ct, clen, pt, sizeof(pt), &plen) == SENSEI_CRYPTO_OK;
  ok &= plen == sizeof(msg) && memcmp(pt, msg, plen) == 0;
  check("ECDH + HKDF + AES-GCM end-to-end", ok);

  sensei_crypto_destroy_key(a_id);
  sensei_crypto_destroy_key(b_id);
  sensei_crypto_destroy_key(a_aes);
  sensei_crypto_destroy_key(b_aes);
}

/* ----------------------------------------------------------------- random */

static void test_random(void) {
  uint8_t a[32] = {0}, b[32] = {0};
  uint8_t zero[32] = {0};

  check("random fills buffers", status_is(sensei_crypto_random(a, sizeof(a)), SENSEI_CRYPTO_OK) &&
                                    status_is(sensei_crypto_random(b, sizeof(b)), SENSEI_CRYPTO_OK));
  check("random output is not constant", memcmp(a, b, sizeof(a)) != 0 && memcmp(a, zero, sizeof(a)) != 0);
}

int run_tests(void) {
  n_checks = 0;
  n_failed = 0;

  printk("\n=== SENSEI crypto tests ===\n");
  test_random();
  test_sha256();
  test_hmac();
  test_hkdf();
  test_aead();
  test_ecdsa();
  test_ecdh();
  test_key_agreement_flow();
  printk("=== %d checks, %d failed ===\n", n_checks, n_failed);

  return n_failed;
}
