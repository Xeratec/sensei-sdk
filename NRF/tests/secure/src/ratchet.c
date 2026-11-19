/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The forward secrecy primitives with the Zephyr backend (X25519, HMAC and HKDF of the SDK crypto library on PSA). The
 * test functions and the vectors are those of the host test (lib/sensei_secure/tests/test_ratchet.c): the file is
 * included, CHECK is zassert, each test function is one test case. */

#include <zephyr/ztest.h>

#define SENSEI_SECURE_TEST_CHECK_H /* use this CHECK instead of check.h */
#define CHECK(cond) zassert_true((cond), #cond)
#define CHECK_DONE() 0
#define main host_main_ratchet                                   /* not used */
#define sensei_secure_crypto_openssl sensei_secure_crypto_zephyr /* in host_main only */

static int check_failures; /* zassert stops at the first failure; only the transcript loop reads this */

#include "test_ratchet.c"

#undef main
#undef sensei_secure_crypto_openssl

static void *setup(void) {
  C = sensei_secure_crypto_zephyr();
  zassert_not_null(C);
  return NULL;
}

ZTEST(sensei_ratchet, test_root0) { test_root0(); }
ZTEST(sensei_ratchet, test_x25519) { test_x25519(); }
ZTEST(sensei_ratchet, test_mix) { test_mix(); }
ZTEST(sensei_ratchet, test_chain) { test_chain(); }
ZTEST(sensei_ratchet, test_epoch_math) { test_epoch_math(); }
ZTEST(sensei_ratchet, test_envelopes) { test_envelopes(); }
ZTEST(sensei_ratchet, test_envelope_tamper) { test_envelope_tamper(); }
ZTEST(sensei_ratchet, test_keyring) { test_keyring(); }
ZTEST(sensei_ratchet, test_tx_jump_persists) { test_tx_jump_persists(); }
ZTEST(sensei_ratchet, test_tx_block) { test_tx_block(); }
ZTEST(sensei_ratchet, test_hook_contract) { test_hook_contract(); }
ZTEST(sensei_ratchet, test_kind_byte) { test_kind_byte(); }
ZTEST(sensei_ratchet, test_transcript) { test_transcript(); }
ZTEST(sensei_ratchet, test_property) { test_property(); }

ZTEST_SUITE(sensei_ratchet, NULL, setup, NULL, NULL, NULL);
