/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The commissioning (sensei_commission.c: SPAKE2+, the kind 5 channel, the verifier record) with the Zephyr backend and
 * the Mbed TLS ECP point arithmetic (CONFIG_SENSEI_SECURE_SPAKE2P_MBEDTLS_ECP, native_sim only). The test functions are
 * those of the host test (lib/sensei_secure/tests/test_commission.c): the file is included, CHECK is zassert, each test
 * function is one test case. test_v9 is the RFC 9383 appendix C exchange on this backend. */

#include <zephyr/ztest.h>

#define SENSEI_SECURE_TEST_CHECK_H /* use this CHECK instead of check.h */
#define CHECK(cond) zassert_true((cond), #cond)
#define CHECK_DONE() 0
#define main host_main_commission                                /* not used */
#define sensei_secure_crypto_openssl sensei_secure_crypto_zephyr /* in host_main only */

#include "test_commission.c"

#undef main
#undef sensei_secure_crypto_openssl

static void *setup_commission(void) {
  C0 = sensei_secure_crypto_zephyr();
  zassert_not_null(C0);
  zassert_not_null(C0->spake2p_zv);
  return NULL;
}

ZTEST(sensei_commission, test_v9) { test_v9(); }
ZTEST(sensei_commission, test_points) { test_points(); }
ZTEST(sensei_commission, test_v10) { test_v10(); }
ZTEST(sensei_commission, test_v11) { test_v11(); }
ZTEST(sensei_commission, test_v12) { test_v12(); }
ZTEST(sensei_commission, test_v13) { test_v13(); }
ZTEST(sensei_commission, test_v14) { test_v14(); }
ZTEST(sensei_commission, test_v15a) { test_v15a(); }
ZTEST(sensei_commission, test_x5_channel) { test_x5_channel(); }
ZTEST(sensei_commission, test_v15b) { test_v15b(); }
ZTEST(sensei_commission, test_v15c) { test_v15c(); }
ZTEST(sensei_commission, test_v15d) { test_v15d(); }
ZTEST(sensei_commission, test_flow) { test_flow(); }
ZTEST(sensei_commission, test_begin_twice) { test_begin_twice(); }
ZTEST(sensei_commission, test_aborts) { test_aborts(); }
ZTEST(sensei_commission, test_tamper) { test_tamper(); }
ZTEST(sensei_commission, test_time) { test_time(); }
ZTEST(sensei_commission, test_counting) { test_counting(); }
ZTEST(sensei_commission, test_backend_faults) { test_backend_faults(); }
ZTEST(sensei_commission, test_garbage) { test_garbage(); }

ZTEST_SUITE(sensei_commission, NULL, setup_commission, NULL, NULL, NULL);
