/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SENSEI crypto library on PSA (Mbed TLS on native_sim). The checks are the known-answer tests of the example
 * crypto_api (NIST, RFC 4231, RFC 5869, RFC 6979, RFC 5903): the file is included, each group is one test case. */

#include <zephyr/ztest.h>

#include "../../../../example/crypto_api/src_NRF/tests.c"

static void *setup(void) {
  zassert_equal(sensei_crypto_init(), SENSEI_CRYPTO_OK);
  return NULL;
}

/* Runs one group and fails if one of its checks failed (the failing check is printed). */
static void run_group(void (*fn)(void)) {
  int before = n_failed;

  fn();
  zassert_equal(n_failed, before, "%d checks failed", n_failed - before);
}

ZTEST(sensei_crypto, test_random) { run_group(test_random); }
ZTEST(sensei_crypto, test_sha256) { run_group(test_sha256); }
ZTEST(sensei_crypto, test_hmac) { run_group(test_hmac); }
ZTEST(sensei_crypto, test_hkdf) { run_group(test_hkdf); }
ZTEST(sensei_crypto, test_aead_gcm) { run_group(test_aead); }
ZTEST(sensei_crypto, test_ecdsa_p256) { run_group(test_ecdsa); }
ZTEST(sensei_crypto, test_ecdh_p256) { run_group(test_ecdh); }
ZTEST(sensei_crypto, test_key_agreement_flow) { run_group(test_key_agreement_flow); }

ZTEST_SUITE(sensei_crypto, NULL, setup, NULL, NULL, NULL);
