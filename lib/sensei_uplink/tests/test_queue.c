/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Message queue: limits, drop policy, wrap-around, flags. */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "sensei_uplink/msg_queue.h"

static void test_queue(void) {
  uint8_t store[64], out[64], m[20];
  struct msg_queue q;

  mq_init(&q, store, sizeof(store));
  CHECK(mq_peek_len(&q) == 0);
  CHECK(mq_push(&q, m, 0) == -EINVAL);
  CHECK(mq_push(&q, m, MQ_MAX_MSG + 1) == -EINVAL);

  /* 64 B holds two 20 B messages (22 B each), a third is refused and counted. */
  for (int i = 0; i < 3; i++) {
    memset(m, i + 1, sizeof(m));
    int e = mq_push(&q, m, sizeof(m));
    CHECK(e == (i < 2 ? 0 : -ENOMEM));
  }
  CHECK(q.count == 2 && q.dropped == 1);
  CHECK(mq_free(&q) == 64 - 2 * 22);
  /* The oldest are kept. */
  CHECK(mq_peek(&q, out, sizeof(out)) == 20 && out[0] == 1);
  mq_pop(&q);
  CHECK(mq_free(&q) == 64 - 22);
  CHECK(mq_peek(&q, out, sizeof(out)) == 20 && out[0] == 2);
  CHECK(mq_peek(&q, out, 10) == 0); /* cap too small */

  mq_clear(&q);
  CHECK(q.count == 0 && q.used == 0);
}

static void test_wrap_content(void) {
  uint8_t store[50], out[40], m[40];
  struct msg_queue q;

  mq_init(&q, store, sizeof(store));
  for (int i = 0; i < 50; i++) {
    size_t len = 10 + (size_t)(i % 25);

    for (size_t k = 0; k < len; k++) {
      m[k] = (uint8_t)(i + k);
    }
    CHECK(mq_push(&q, m, len) == 0);
    CHECK(mq_peek(&q, out, sizeof(out)) == len);
    CHECK(memcmp(out, m, len) == 0);
    mq_pop(&q);
  }
}

/* The flag is kept with the message, also over the wrap, and does not change the length. */
static void test_flag(void) {
  uint8_t store[50], out[40], m[30];
  struct msg_queue q;

  mq_init(&q, store, sizeof(store));
  for (size_t i = 0; i < sizeof(m); i++) {
    m[i] = (uint8_t)i;
  }
  for (int round = 0; round < 5; round++) {
    CHECK(mq_push_flag(&q, m, sizeof(m), true) == 0);
    CHECK(mq_peek_flag(&q) && mq_peek_len(&q) == sizeof(m));
    CHECK(mq_peek(&q, out, sizeof(out)) == sizeof(m) && memcmp(out, m, sizeof(m)) == 0);
    mq_pop(&q);
    CHECK(mq_push(&q, m, 10) == 0 && !mq_peek_flag(&q) && mq_peek_len(&q) == 10);
    mq_pop(&q);
  }
  CHECK(!mq_peek_flag(&q));
}

/* Both flags, the length of a large message, and flags that do not mix. */
static void test_flags(void) {
  static uint8_t store[6000], big[MQ_MAX_MSG], out[MQ_MAX_MSG];
  uint8_t m[20] = {1, 2, 3};
  struct msg_queue q;
  static const uint8_t all[] = {0, MQ_FLAG_PLAIN, MQ_FLAG_REPORT, MQ_FLAG_PLAIN | MQ_FLAG_REPORT};

  mq_init(&q, store, sizeof(store));
  for (size_t i = 0; i < sizeof(big); i++) {
    big[i] = (uint8_t)(i * 3);
  }
  for (size_t i = 0; i < sizeof(all); i++) {
    CHECK(mq_push_flags(&q, m, sizeof(m), all[i]) == 0);
    CHECK(mq_peek_flags(&q) == all[i] && mq_peek_len(&q) == sizeof(m));
    CHECK(mq_peek_flag(&q) == ((all[i] & MQ_FLAG_PLAIN) != 0));
    mq_pop(&q);
    CHECK(mq_peek_flags(&q) == 0);
  }
  CHECK(mq_push_flags(&q, big, sizeof(big), MQ_FLAG_REPORT) == 0);
  CHECK(mq_peek_len(&q) == MQ_MAX_MSG && mq_peek_flags(&q) == MQ_FLAG_REPORT);
  CHECK(mq_peek(&q, out, sizeof(out)) == sizeof(big) && memcmp(out, big, sizeof(big)) == 0);
}

int main(void) {
  test_queue();
  test_wrap_content();
  test_flag();
  test_flags();
  DONE();
}
