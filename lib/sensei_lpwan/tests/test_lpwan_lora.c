/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* LoRa payload limit from the data rate (EU868), airtime, send time limits, and a model of the LR1121 duty-cycle wait
 * for a message of several chunks (D2). */

#include <stdint.h>
#include <stdio.h>

#include "sensei_lpwan/lpwan_lora.h"

static int failures;
#define CHECK(c)                                                                                                       \
  do {                                                                                                                 \
    if (!(c)) {                                                                                                        \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                                     \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

/* ---- Fake LR1121 timing ----
 * EU868 sub-band g1 allows 1 % airtime: after a frame of airtime T the LR1121 keeps the next one back for 99 T (the
 * Modem-E firmware queues it, get_duty_cycle_status() reports the wait as a negative number). TX_DONE comes after the
 * receive windows (RX1 at 1 s, RX2 at 2 s, and the window itself). */
#define FAKE_RX_MS 2100u

struct fake_lr {
  uint32_t now_ms;
  uint32_t blocked_until_ms;
  uint8_t dr;
};

static int32_t fake_duty_status(const struct fake_lr *f) {
  return f->now_ms >= f->blocked_until_ms ? 0 : -(int32_t)(f->blocked_until_ms - f->now_ms);
}

/* Queue an uplink of `app_len` bytes now. Returns the time at which TX_DONE comes. */
static uint32_t fake_request_tx(struct fake_lr *f, uint32_t app_len) {
  uint32_t start = f->now_ms > f->blocked_until_ms ? f->now_ms : f->blocked_until_ms;
  uint32_t air = lpwan_lora_airtime_ms(f->dr, app_len + 13u); /* what the real frame has: no FOpts */

  f->blocked_until_ms = start + 99u * air;
  return start + air + FAKE_RX_MS;
}

/*
 * A message of `msg_len` bytes in chunks of the data rate's size, sent one chunk after the other (the next goes out
 * when the previous tx_done is in), as the node's uplink scheduler does. For every chunk the nRF9151 reads the wait
 * from the LR1121 and returns the expected time; its watchdog and the client's deadline follow from it.
 * Returns the number of chunks the old fixed 45 s watchdog would have failed.
 */
static unsigned model_message(uint8_t dr, bool adr, uint32_t msg_len, unsigned chunks_expected) {
  /* with ADR the estimate is for DR0, the real rate is higher: only "never early" holds */
  struct fake_lr f = {.dr = dr};
  uint32_t chunk = lpwan_lora_chunk_max(dr, adr, 0);
  uint32_t left = msg_len;
  unsigned n = 0, old_watchdog_fails = 0;

  while (left > 0) {
    uint32_t len = left < chunk ? left : chunk;
    int32_t duty = fake_duty_status(&f);
    uint32_t wait = duty < 0 ? (uint32_t)-duty : 0;
    uint32_t expected = lpwan_lora_expected_ms(dr, adr, len, wait);
    uint32_t shield_limit = lpwan_send_limit_ms(expected, 45000, 5000);
    uint32_t client_limit = lpwan_send_limit_ms(expected, 60000, 20000);
    uint32_t t_send = f.now_ms;
    uint32_t actual = fake_request_tx(&f, len) - t_send;

    CHECK(expected >= actual);                   /* never early: the watchdog would fail a good send */
    CHECK(adr || expected - actual <= 2500);     /* and not much later */
    CHECK(shield_limit >= actual);               /* the nRF9151 waits for the real result */
    CHECK(client_limit >= shield_limit + 15000); /* and ends the send first */
    CHECK(client_limit <= LPWAN_SEND_MAX_MS + 20000);
    old_watchdog_fails += actual > 45000;
    f.now_ms = t_send + actual; /* tx_done is in: the next chunk */
    left -= len;
    n++;
  }
  CHECK(n == chunks_expected);
  return old_watchdog_fails;
}

static void test_duty_cycle_model(void) {
  /* a sealed status of 76 B at DR0 (51 B chunks) and a 160 B message at DR3 (115 B chunks): two chunks each. The
   * second waits for the duty cycle for minutes at DR0 and a minute at DR3; the old 45 s watchdog failed it. */
  CHECK(model_message(0, false, 76, 2) == 1);
  CHECK(model_message(3, false, 160, 2) == 1);
  /* three chunks at DR0: two of them wait */
  CHECK(model_message(0, false, 140, 3) == 2);
  /* with ADR the chunks are 51 B whatever the rate, the estimate uses DR0: never early */
  CHECK(model_message(3, true, 76, 2) >= 0);
  /* DR5: one chunk of 222 B, no waiting, nothing to fail */
  CHECK(model_message(5, false, 222, 1) == 0);
  /* DR4 and up: a 300 B message is two chunks, the wait is a few seconds */
  CHECK(model_message(5, false, 300, 2) == 0);

  /* the wait the LR1121 reports is what the second chunk of DR0 needs: about 99 times the first airtime */
  {
    struct fake_lr f = {.dr = 0};
    uint32_t done = fake_request_tx(&f, 51);

    f.now_ms = done;
    CHECK(fake_duty_status(&f) < -265000 && fake_duty_status(&f) > -280000); /* 99 x 2.79 s, minus the time since */
  }
}

static void test_airtime(void) {
  /* 64 B frame (51 B payload plus 13 B LoRaWAN): known values for EU868, 125 kHz */
  CHECK(lpwan_lora_airtime_ms(0, 64) == 2794); /* SF12: 2793.5 ms */
  CHECK(lpwan_lora_airtime_ms(1, 64) == 1561);
  CHECK(lpwan_lora_airtime_ms(5, 64) >= 118 && lpwan_lora_airtime_ms(5, 64) <= 119);
  CHECK(lpwan_lora_airtime_ms(6, 64) >= 59 && lpwan_lora_airtime_ms(6, 64) <= 60); /* SF7, 250 kHz */
  CHECK(lpwan_lora_airtime_ms(7, 64) == 12);                                       /* FSK 50 kbit/s */
  /* longer frames take longer, a lower rate takes longer */
  for (unsigned dr = 0; dr < 6; dr++) {
    CHECK(lpwan_lora_airtime_ms((uint8_t)dr, 100) > lpwan_lora_airtime_ms((uint8_t)dr, 20));
    CHECK(lpwan_lora_airtime_ms((uint8_t)dr, 50) > lpwan_lora_airtime_ms((uint8_t)dr + 1, 50));
  }
  CHECK(lpwan_lora_airtime_ms(9, 64) == lpwan_lora_airtime_ms(0, 64)); /* not an EU868 rate: as DR0 */
  CHECK(lpwan_lora_airtime_ms(0, 0) > 0);
}

static void test_chunk_and_limits(void) {
  /* the LR1121 reports nothing, or room for a full chunk: the nominal size */
  CHECK(lpwan_lora_chunk_max(0, false, 0) == 51);
  CHECK(lpwan_lora_chunk_max(0, false, 51) == 51);
  CHECK(lpwan_lora_chunk_max(3, false, 222) == 115);
  /* MAC answers pending (FOpts): what is left, minus a margin for more */
  CHECK(lpwan_lora_chunk_max(0, false, 36) == 36 - LPWAN_LORA_MAC_MARGIN);
  CHECK(lpwan_lora_chunk_max(3, false, 100) == 100 - LPWAN_LORA_MAC_MARGIN);
  CHECK(lpwan_lora_chunk_max(5, true, 40) == 25); /* ADR: nominal 51 */
  CHECK(lpwan_lora_chunk_max(0, false, 5) == 4);  /* the smallest chunk the fragmenter takes */
  /* the chunk is never above the nominal value */
  for (unsigned nm = 0; nm < 256; nm++) {
    CHECK(lpwan_lora_chunk_max(0, false, nm) <= 51 && lpwan_lora_chunk_max(0, false, nm) >= 4);
  }

  /* expected time: wait + airtime + receive windows, capped */
  CHECK(lpwan_lora_expected_ms(5, false, 10, 0) >= LPWAN_LORA_RX_MS);
  CHECK(lpwan_lora_expected_ms(5, false, 10, 1000) == lpwan_lora_expected_ms(5, false, 10, 0) + 1000);
  CHECK(lpwan_lora_expected_ms(5, false, 10, 4000000) == LPWAN_SEND_MAX_MS);
  CHECK(lpwan_lora_expected_ms(5, true, 10, 0) == lpwan_lora_expected_ms(0, false, 10, 0)); /* ADR: as DR0 */

  /* limits: the floor without an estimate, estimate plus extra with one, never above the cap plus extra */
  CHECK(lpwan_send_limit_ms(0, 45000, 5000) == 45000);
  CHECK(lpwan_send_limit_ms(0, 60000, 20000) == 60000);
  CHECK(lpwan_send_limit_ms(100000, 45000, 5000) == 105000);
  CHECK(lpwan_send_limit_ms(100000, 60000, 20000) == 120000);
  CHECK(lpwan_send_limit_ms(UINT32_MAX, 45000, 5000) == LPWAN_SEND_MAX_MS + 5000);
}

int main(void) {
  test_airtime();
  test_chunk_and_limits();
  test_duty_cycle_model();
  /* DR0 to DR2: 51 B, DR3: 115 B, DR4 and up: 222 B */
  CHECK(lpwan_lora_max_payload(0, false) == 51);
  CHECK(lpwan_lora_max_payload(2, false) == 51);
  CHECK(lpwan_lora_max_payload(3, false) == 115);
  CHECK(lpwan_lora_max_payload(4, false) == 222);
  CHECK(lpwan_lora_max_payload(5, false) == 222);
  CHECK(lpwan_lora_max_payload(7, false) == 222);
  /* not an EU868 rate: the safe value */
  CHECK(lpwan_lora_max_payload(8, false) == 51);
  CHECK(lpwan_lora_max_payload(15, false) == 51);
  /* ADR: the network may lower the rate to DR0 whatever we configured */
  for (unsigned dr = 0; dr <= 15; dr++) {
    CHECK(lpwan_lora_max_payload((uint8_t)dr, true) == 51);
  }
  /* The node chunks by this value: a chunk of 3 B header and the payload must fit one uplink */
  CHECK(lpwan_lora_max_payload(5, false) <= 222);

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
