/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Recovery over the SPI link (the production build). GAP9 is the SPI master: the HELLO tells the nRF5340 that this is
 * the boot loader (major 0xFFFF), then the jobs are fetched and answered as the app's job server does (gap9_side.c).
 * The SPI runs at the clock of the SSBL, 2 MHz. Does not return: the nRF5340 decides (apply powers GAP9 off and on
 * again with the slot line at the new slot). Untested on hardware. */

#include <errno.h>

#include <pmsis.h>

#include "recovery.h"
#include "sensei_gap9/gap9_link.h"
#include "sensei_gap9/gap9_side.h"

#ifndef SENSEI_SSBL_GIT
#define SENSEI_SSBL_GIT 0
#endif

int recovery_serve(struct g9rec *r, uint32_t reason) {
  static sensei_Gap9RecoveryJob job;
  static sensei_Gap9RecoveryResult res;
  static uint8_t buf[sensei_Gap9RecoveryResult_size + 16];
  const struct g9l_cfg lc = {
      .pad_sck = G9L_PAD_SCK,
      .pad_mosi = G9L_PAD_MOSI,
      .pad_miso = G9L_PAD_MISO,
      .pad_cs = G9L_PAD_CS,
      .pad_trigger = G9L_PAD_TRIGGER,
      .spi_hz = 0, /* G9L_SPI_HZ_DEFAULT */
      .spi_itf = G9L_SPI_ITF,
  };
  const sensei_Version hello = {G9L_RECOVERY_MAJOR, G9L_RECOVERY_PROTO, reason, SENSEI_SSBL_GIT};

  if (g9l_init(&lc) != 0) {
    return -EIO;
  }
  /* The nRF5340 arms its side when it powers GAP9: if it is not armed yet the push fails, try again. */
  while (g9l_hello(&hello) != 0) {
    pi_time_wait_us(100000);
  }
  for (;;) {
    int rc = g9l_fetch(g9rec_decode, &job);

    if (rc == G9L_FETCH_JOB) {
      int32_t n;

      (void)g9rec_run(r, &job, pi_time_get_us() / 1000u, &res);
      n = g9rec_encode(&res, buf, sizeof(buf));
      if (n > 0) {
        (void)g9l_send_result(buf, (size_t)n, false);
      }
    } else if (rc == G9L_FETCH_NONE) {
      g9l_wait_work();
    } else {
      pi_time_wait_us(1000); /* a bad frame or no armed line: try again */
    }
  }
  return 0;
}
