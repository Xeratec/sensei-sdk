/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* The device key store (sensei_e2e.c: state machine of the mix, counter blocks, power-loss replay) with the Zephyr
 * backend. The test functions are those of the host test (lib/sensei_secure/tests/test_e2e.c): the file is included,
 * CHECK is zassert, each test function is one test case. */

#include <zephyr/ztest.h>

#define SENSEI_SECURE_TEST_CHECK_H /* use this CHECK instead of check.h */
#define CHECK(cond) zassert_true((cond), #cond)
#define CHECK_DONE() 0
#define main host_main_e2e                                       /* not used */
#define sensei_secure_crypto_openssl sensei_secure_crypto_zephyr /* in host_main only */

static int check_failures; /* zassert stops at the first failure; only the transcript loop reads this */

#include "test_e2e.c"

#undef main
#undef sensei_secure_crypto_openssl

static void *setup_e2e(void) {
  C = sensei_secure_crypto_zephyr();
  zassert_not_null(C);
  return NULL;
}

ZTEST(sensei_e2e, test_first_start) { test_first_start(); }
ZTEST(sensei_e2e, test_provision_and_traffic) { test_provision_and_traffic(); }
ZTEST(sensei_e2e, test_key_a_b_a) { test_key_a_b_a(); }
ZTEST(sensei_e2e, test_two_senders) { test_two_senders(); }
ZTEST(sensei_e2e, test_rx_store_failure) { test_rx_store_failure(); }
ZTEST(sensei_e2e, test_sender_table_full) { test_sender_table_full(); }
ZTEST(sensei_e2e, test_clear_senders) { test_clear_senders(); }
ZTEST(sensei_e2e, test_replay_after_clear) { test_replay_after_clear(); }
ZTEST(sensei_e2e, test_reset) { test_reset(); }
ZTEST(sensei_e2e, test_reset_refused_when_erase_fails) { test_reset_refused_when_erase_fails(); }
ZTEST(sensei_e2e, test_priv_valid) { test_priv_valid(); }
ZTEST(sensei_e2e, test_damaged_key) { test_damaged_key(); }
ZTEST(sensei_e2e, test_sealed_key_crypto_error) { test_sealed_key_crypto_error(); }
ZTEST(sensei_e2e, test_key_unreadable) { test_key_unreadable(); }
ZTEST(sensei_e2e, test_key_store_failure) { test_key_store_failure(); }
ZTEST(sensei_e2e, test_reset_then_provision_is_refused_while_dirty) {
  test_reset_then_provision_is_refused_while_dirty();
}
ZTEST(sensei_e2e, test_fs_full_mix) { test_fs_full_mix(); }
ZTEST(sensei_e2e, test_fs_answers) { test_fs_answers(); }
ZTEST(sensei_e2e, test_fs_failing_writes) { test_fs_failing_writes(); }
ZTEST(sensei_e2e, test_fs_blocks) { test_fs_blocks(); }
ZTEST(sensei_e2e, test_fs_epoch_time) { test_fs_epoch_time(); }
ZTEST(sensei_e2e, test_fs_triggers) { test_fs_triggers(); }
ZTEST(sensei_e2e, test_fs_mark_ahead) { test_fs_mark_ahead(); }
ZTEST(sensei_e2e, test_fs_reprovision) { test_fs_reprovision(); }
ZTEST(sensei_e2e, test_fs_damaged) { test_fs_damaged(); }
ZTEST(sensei_e2e, test_fs_transcript) { test_fs_transcript(); }
ZTEST(sensei_e2e, test_fs_power_loss) { test_fs_power_loss(); }

ZTEST_SUITE(sensei_e2e, NULL, setup_e2e, NULL, NULL, NULL);
