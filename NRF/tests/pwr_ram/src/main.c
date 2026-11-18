/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_pwr_ram_powerdown() (NRF/system/pwr/pwr_ram.c): it calls the nRF Connect SDK library once and reports the
 * RAM it covers, the 4 kB sections from the end of the image RAM to the end of the chosen zephyr,sram (the library
 * knows no other region). The library is a fake here. The build-time checks of the file (every memory region lies
 * above zephyr,sram) pass for this devicetree; a board that breaks them does not build. */

#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include "pwr/pwr_ram.h"

DEFINE_FFF_GLOBALS;

FAKE_VOID_FUNC(power_down_unused_ram);

#define SRAM_END 0x20040000UL
#define SECTION 0x1000UL

ZTEST(pwr_ram, test_01_covered_ram) {
  size_t n = sensei_pwr_ram_powerdown();
  unsigned long image_end = CONFIG_SENSEI_TEST_IMAGE_END;
  unsigned long first = (image_end + SECTION - 1) / SECTION * SECTION; /* whole sections only */

  zassert_equal(power_down_unused_ram_fake.call_count, 1, "the library is called once");
  zassert_equal(n, first < SRAM_END ? SRAM_END - first : 0);
#if CONFIG_SENSEI_TEST_IMAGE_END == 0x20003210
  zassert_equal(n, 0x3c000, "from 0x20004000 to 0x20040000");
#else
  zassert_equal(n, 0, "no section left, and no wrap-around");
#endif
}

ZTEST_SUITE(pwr_ram, NULL, NULL, NULL, NULL, NULL);
