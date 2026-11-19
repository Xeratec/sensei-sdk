/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_secure with the Zephyr backend (SDK crypto library on PSA). The test functions and the vectors are those
 * of the host test with OpenSSL (lib/sensei_secure/tests): the file is included, CHECK is zassert, each test
 * function is one test case. The Python vectors must come out the same on both backends. */

#include <zephyr/ztest.h>

#define SENSEI_SECURE_TEST_CHECK_H /* use this CHECK instead of check.h */
#define CHECK(cond) zassert_true((cond), #cond)
#define CHECK_DONE() 0
#define main host_main                                           /* not used */
#define sensei_secure_crypto_openssl sensei_secure_crypto_zephyr /* in host_main only */

#include "test_secure.c"

#undef main
#undef sensei_secure_crypto_openssl

static void *setup(void) {
  C = sensei_secure_crypto_zephyr();
  zassert_not_null(C);
  return NULL;
}

ZTEST(sensei_secure, test_keys) { test_keys(); }
ZTEST(sensei_secure, test_vectors) { test_vectors(); }
ZTEST(sensei_secure, test_tamper) { test_tamper(); }
ZTEST(sensei_secure, test_errors) { test_errors(); }
ZTEST(sensei_secure, test_replay_window) { test_replay_window(); }
ZTEST(sensei_secure, test_counters) { test_counters(); }
ZTEST(sensei_secure, test_senders) { test_senders(); }
ZTEST(sensei_secure, test_sender_table) { test_sender_table(); }
ZTEST(sensei_secure, test_init) { test_init(); }
ZTEST(sensei_secure, test_is_sealed_markers) { test_is_sealed_markers(); }
ZTEST(sensei_secure, test_fresh_keys) { test_fresh_keys(); }
ZTEST(sensei_secure, test_generation_bits) { test_generation_bits(); }

ZTEST_SUITE(sensei_secure, NULL, setup, NULL, NULL, NULL);
