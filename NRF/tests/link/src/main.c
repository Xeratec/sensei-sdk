/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_link as Zephyr library: the generated messages and the pb glue on the nanopb runtime of NCS (a version
 * mismatch is a compile error), the framing, SPI frame and fragmentation code, and the Kconfig option
 * CONFIG_SENSEI_SPI_MAX_PAYLOAD. The test functions are those of the host tests (lib/sensei_link/tests): the files
 * are included and each function is one test case. */

#include <zephyr/ztest.h>

#define main framing_main /* not used */
#include "test_framing.c"
#undef main

#define main pb_main /* not used */
#include "test_pb.c"
#undef main

/* Runs a host test function and fails if one of its CHECKs failed (the failing line is printed). */
static void run(void (*fn)(void)) {
  int before = check_failures;

  fn();
  zassert_equal(check_failures, before, "%d checks failed", check_failures - before);
}

ZTEST(sensei_link, test_crc) { run(test_crc); }
ZTEST(sensei_link, test_cobs) { run(test_cobs); }
ZTEST(sensei_link, test_stream) { run(test_stream); }
ZTEST(sensei_link, test_spi) { run(test_spi); }
ZTEST(sensei_link, test_spi_header_crc) { run(test_spi_header_crc); }
ZTEST(sensei_link, test_frag) { run(test_frag); }
ZTEST(sensei_link, test_pb_version) { run(test_version); }
ZTEST(sensei_link, test_pb_lpwan) { run(test_lpwan); }
ZTEST(sensei_link, test_pb_limits) { run(test_limits); }
ZTEST(sensei_link, test_pb_garbage) { run(test_garbage); }

/* The Kconfig value reaches the library (prj.conf sets 4096, the default is 8192). */
ZTEST(sensei_link, test_max_payload_option) {
  zassert_equal(SENSEI_SPI_MAX_PAYLOAD, CONFIG_SENSEI_SPI_MAX_PAYLOAD);
  zassert_equal(SENSEI_SPI_MAX_PAYLOAD, 4096);
}

ZTEST_SUITE(sensei_link, NULL, NULL, NULL, NULL, NULL);
