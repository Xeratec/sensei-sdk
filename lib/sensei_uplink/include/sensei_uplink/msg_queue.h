/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file msg_queue.h
 * @brief Byte-bounded FIFO of variable-size messages. No Zephyr, no locking.
 *
 * Each entry is stored as a 2 B header (13 bit length, bits 15 to 13 flags) and the data in a
 * ring. A full queue refuses new entries (the oldest are kept) and counts them. A user that prefers
 * the newest (the uplink scheduler) checks mq_free() and drops the oldest with mq_pop() first.
 */

#ifndef SENSEI_UPLINK_MSG_QUEUE_H_
#define SENSEI_UPLINK_MSG_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQ_HDR 2u
#define MQ_MAX_MSG 4096u

/* Flags kept with a message (the uplink scheduler's). */
#define MQ_FLAG_PLAIN 0x01u  /* an unsealed message: ops->prepare may change it for the radio */
#define MQ_FLAG_REPORT 0x02u /* the app is told when it is sent or dropped (us_ops.msg_end) */
#define MQ_FLAG_REKEY                                                                                                  \
  0x04u /* holds a key mix request: us_ops.msg_end is called too (the node asks again if it is dropped) */

struct msg_queue {
  uint8_t *buf;
  size_t cap;
  size_t head; /* read offset */
  size_t tail; /* write offset */
  size_t used; /* bytes, headers included */
  size_t count;
  uint32_t dropped;
};

void mq_init(struct msg_queue *q, uint8_t *buf, size_t cap);

/** Append. -EINVAL for an empty or too long message, -ENOMEM if it does not fit (counted). */
int mq_push(struct msg_queue *q, const uint8_t *msg, size_t len);

/** Like mq_push() with a flag kept with the message (the scheduler: a plain, unsealed Uplink). */
int mq_push_flag(struct msg_queue *q, const uint8_t *msg, size_t len, bool flag);

/** Like mq_push() with MQ_FLAG_* bits. */
int mq_push_flags(struct msg_queue *q, const uint8_t *msg, size_t len, uint8_t flags);

/** The flag (MQ_FLAG_PLAIN) of the oldest message, false if empty. */
bool mq_peek_flag(const struct msg_queue *q);

/** The MQ_FLAG_* bits of the oldest message, 0 if empty. */
uint8_t mq_peek_flags(const struct msg_queue *q);

/** Length of the oldest message, 0 if empty. */
size_t mq_peek_len(const struct msg_queue *q);

/** Free bytes (a message of len needs len + MQ_HDR). */
size_t mq_free(const struct msg_queue *q);

/** Copy the oldest message to out. Returns its length, 0 if empty or cap too small. */
size_t mq_peek(const struct msg_queue *q, uint8_t *out, size_t cap);

/** Remove the oldest message. */
void mq_pop(struct msg_queue *q);

void mq_clear(struct msg_queue *q);

#endif /* SENSEI_UPLINK_MSG_QUEUE_H_ */
