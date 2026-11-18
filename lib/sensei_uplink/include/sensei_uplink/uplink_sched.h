/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file uplink_sched.h
 * @brief Uplink queue, radio choice, fragmentation, completion-driven sending and retries.
 *        No Zephyr, no locking. Time, timers and error counters come in through ops, so the
 *        logic runs on the host with a fake radio and clock.
 *
 * Two queues: alerts and status (own room, sent first) and data. A full queue drops its oldest
 * messages to make room, so a radio that is down for days keeps the newest data and never blocks
 * an alert.
 *
 * Sending: take the oldest message, split it with sensei_frag to the radio MTU, send one chunk
 * at a time. The next chunk goes out when the radio calls done(). A failed chunk is retried
 * `retries` times with a growing delay and random jitter. A chunk that is not completed within
 * US_CHUNK_TIMEOUT_MS counts as failed (a radio can set its own limit; LoRa waits for the duty
 * cycle). If all retries fail the session ends and the message stays queued for the next uplink
 * time; after US_MAX_SESSION_FAILS failed sessions it is dropped. A completion that belongs to an
 * earlier send (after an abort, a timeout or a radio switch) is ignored: the radio gets the id of
 * the send as `user`. One scheduler per program.
 *
 * The chunk size is read once per message (uplink_radio.mtu_now, else mtu). If a radio reports
 * -EMSGSIZE for a chunk and the size did change, the message starts again with the new size (new
 * message id), and that counts as one attempt. If the size did not change, it is an ordinary
 * failed chunk (retry with backoff). Both count for a synchronous refusal of send() too.
 *
 * -EAGAIN (from send() or done()) is a deferral: the radio cannot send now (LoRa duty cycle, a
 * shield update) and calls its up hook when it can (uplink_flush). The session ends, the message
 * stays, no attempt and no failed session is counted.
 *
 * -ENOTCONN is a deferral too, but it means the shield restarted or lost its connection and
 * connects again (on_up follows). It is counted per head message (conn_defers, reset only when a
 * chunk succeeds) and the attempts are kept: after US_MAX_CONN_DEFERS the session counts as failed
 * (session_failed) and the message is dropped after the usual number of failed sessions. A shield
 * that resets on every send does not block the queue for ever.
 *
 * After a deferral in the middle of a message, the next session goes on with the refused chunk
 * (same message id) only if the radio keeps the collector's reassembly state across the pause
 * (uplink_radio.keeps_reasm: LoRa) and the last chunk went out less than US_RESUME_MAX_MS ago.
 * Else the message starts again at chunk 0 with a new message id (LTE: the next connection comes
 * from another address, the collector would never see the chunks before). US_RESUME_MAX_MS (300 s) is
 * below the collector's LORA_REASSEMBLY_TIMEOUT (700 s in the SENSEI collector): keep it so.
 *
 * A plain message (us_submit_plain) goes through ops->prepare when it starts, if the radio asks for
 * it (uplink_radio.node_id_prefix). The node adds Uplink.node_id there, for the radio the message
 * is sent on.
 *
 * The radio is told which chunk is the last of the session (uplink_radio.send_last): the last chunk
 * of the last queued message, with both queues empty behind it. LTE sets the release assistance
 * indication on it (the modem leaves the connected state at once) and the collector answers it (ack
 * or downlink). The other chunks, also the last chunk of a message that more follow, go without it,
 * so a queue of messages pays one connection set-up.
 *
 * "Sent, unconfirmed": the radio completes the last chunk with US_SENT_UNCONFIRMED when it went out
 * but the answer did not come (lost, or too slow for the radio's wait). That is not a failure: the
 * scheduler sends that chunk once more in the same session (the collector answers a repeated last
 * chunk again and does not store the message twice, see the collector README). If that one is not
 * answered either, the message counts as delivered without confirmation (stats.unconfirmed) and
 * the next message goes. Only a chunk that did not go out (an error below 0) is a failed chunk,
 * retried with backoff, and after US_MAX_SESSION_FAILS sessions the message is dropped.
 *
 * Deferral on a bad link (us_set_defer, uplink_radio.link_check): before a data message starts the
 * radio may say its link is bad (LTE: connection evaluation). The message then stays queued, up to
 * max_ms from the first deferral, and is tried again every US_DEFER_RECHECK_MS. While the radio is
 * still evaluating (US_LINK_WAIT) the wait counts against the same time: when it is over, a radio
 * that has not answered counts as OK and the message goes. Never deferred: alerts and status (the
 * alert queue), a session that an alert opened (the connection is up anyway), and anything while
 * the data queue is half full or more (nothing is dropped to wait for a better link). The first
 * message of a session that passed the check, or ran out of its time, starts the session: the rest
 * of the queue follows without a check. The wait ends when the data queue is empty.
 *
 * A message can carry MQ_FLAG_REPORT (us_submit_report): ops->msg_end() tells the app when it was
 * sent or dropped (the fault report counts there).
 */

#ifndef SENSEI_UPLINK_SCHED_H_
#define SENSEI_UPLINK_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/types.pb.h"
#include "sensei_uplink/msg_queue.h"
#include "sensei_uplink/uplink_radio.h"

/* Sizes of the buffers inside struct uplink_sched: Kconfig on Zephyr, else the defaults (a host build can
 * define them for the whole target). All code that includes this header must see the same values. */
#ifdef CONFIG_SENSEI_UPLINK_MAX_RADIOS
#define US_MAX_RADIOS CONFIG_SENSEI_UPLINK_MAX_RADIOS
#else
#define US_MAX_RADIOS 4
#endif
#ifdef CONFIG_SENSEI_UPLINK_MAX_MSG
#define US_MAX_MSG CONFIG_SENSEI_UPLINK_MAX_MSG /* largest message in a queue (the node: an Uplink, sealed or not) */
#else
#define US_MAX_MSG 2144u
#endif
#ifdef CONFIG_SENSEI_UPLINK_PREPARE_ROOM
#define US_PREPARE_ROOM CONFIG_SENSEI_UPLINK_PREPARE_ROOM /* what ops->prepare may add to a message */
#else
#define US_PREPARE_ROOM 9u
#endif
#ifdef CONFIG_SENSEI_UPLINK_MAX_MTU
#define US_MAX_MTU CONFIG_SENSEI_UPLINK_MAX_MTU /* largest chunk of any radio */
#else
#define US_MAX_MTU 1024u
#endif
#define US_MAX_SESSION_FAILS 3
/* -ENOTCONN deferrals without progress before a session counts as failed */
#define US_MAX_CONN_DEFERS 3
/* resume a deferred message at the refused chunk only this soon after its last chunk */
#define US_RESUME_MAX_MS 300000u
#define US_RETRY_BASE_MS 2000u
#define US_RETRY_MAX_MS 60000u
#define US_CHUNK_TIMEOUT_MS 90000u  /* a chunk not done after this long failed (radio: chunk_timeout_ms) */
#define US_DEFER_RECHECK_MS 900000u /* a deferred queue asks the radio again after this long */
#define US_LINK_WAIT_MS 35000u      /* the radio evaluates: look again if its up hook did not come */
#define US_STALL_MS                                                                                                    \
  900000u /* a session without progress this long is stuck (watchdog); above the LoRa chunk                            \
             timeout */

enum us_error {
  US_ERR_QUEUE_FULL,
  US_ERR_RADIO, /* no radio, or a message could not be sent */
};

struct uplink_sched;

struct us_ops {
  uint32_t (*now_ms)(void *ctx);
  /** Call us_run() after delay_ms. Replaces an earlier request. */
  void (*schedule)(void *ctx, uint32_t delay_ms);
  void (*error)(void *ctx, enum us_error e);
  /** Random number for retry jitter. Optional (NULL: no jitter). */
  uint32_t (*rand)(void *ctx);
  /** Optional: a message with MQ_FLAG_* flags left the queue. delivered: it was sent (also
   * unconfirmed); else it was dropped (evicted, too large, too many failed sessions). */
  void (*msg_end)(void *ctx, uint8_t flags, bool delivered);
  /** Optional: change a message in buf (len bytes, room for cap) when it is loaded to be sent. Called for a message
   * with MQ_FLAG_PLAIN on a radio with node_id_prefix, each time the message is loaded from the queue (the queued copy
   * stays as it was). Returns the new length (at most cap), or 0 if it cannot be done (the message is dropped). NULL:
   * no change. */
  size_t (*prepare)(void *ctx, uint8_t *buf, size_t len, size_t cap, const struct uplink_radio *radio);
};

enum us_state { US_IDLE, US_SENDING, US_BACKOFF };

struct us_stats {
  uint32_t msgs_sent;
  uint32_t chunks_sent;
  uint32_t retries;
  uint32_t msgs_dropped;       /* gave up after repeated failed sessions, or too large */
  uint32_t msgs_evicted;       /* oldest message dropped to make room */
  uint32_t timeouts;           /* chunks the radio did not complete in time */
  uint32_t unconfirmed;        /* messages sent whose answer did not come (also after the retry) */
  uint32_t unconf_retries;     /* last chunks sent again because the answer did not come */
  uint32_t deferred;           /* sends the radio put off (-EAGAIN, -ENOTCONN) */
  uint32_t conn_deferred;      /* of those: -ENOTCONN (the shield restarted) */
  uint32_t restarted;          /* deferred messages that started again at chunk 0 (new id) */
  uint32_t link_deferred;      /* data holds on a bad link (US_LINK_BAD) */
  uint32_t link_waits;         /* starts that waited for the radio's link check (US_LINK_WAIT) */
  uint32_t link_wait_timeouts; /* checks that were still not answered when the deferral time ended */
};

struct uplink_sched {
  struct msg_queue q;  /* data */
  struct msg_queue qa; /* alerts and status */
  struct msg_queue *cur_q;
  const struct uplink_radio *radios[US_MAX_RADIOS];
  size_t n_radios;
  sensei_Radio radio;
  uint8_t retries;
  const struct us_ops *ops;
  void *ctx;

  enum us_state state;
  const struct uplink_radio *cur_radio;
  size_t cur_mtu;          /* the radio's chunk size when the message started; fixed for all its chunks */
  uint32_t cur_timeout_ms; /* chunk timeout of the radio of this session */
  uint8_t cur[US_MAX_MSG + US_PREPARE_ROOM];
  size_t cur_len;
  uint8_t chunk[US_MAX_MTU];
  uint8_t msg_id;
  size_t chunk_idx;
  size_t chunk_cnt;
  uint8_t attempts;
  uint8_t head_fails;
  bool unconf_retried; /* the last chunk of this message was sent again after "unconfirmed" */
  /* A deferral in the middle of a message: the next session goes on with the chunk that was
   * refused if the head of the queue is still that message (same id, size, chunking and content),
   * the radio keeps the collector's state (keeps_reasm) and resume_ms is recent. Else it starts
   * again from chunk 0 with a new message id. */
  bool resume;
  const struct uplink_radio *resume_radio;
  uint8_t resume_msg_id;
  size_t resume_idx, resume_cnt, resume_len, resume_mtu;
  uint32_t resume_hash;
  uint32_t resume_ms;       /* when the last chunk of the deferred message went out */
  uint32_t last_ok_ms;      /* when the last chunk of the message in work went out */
  uint8_t conn_defers;      /* -ENOTCONN deferrals of the head message since its last progress */
  struct msg_queue *fail_q; /* queue whose head head_fails counts */
  uint32_t send_id;         /* id of the current send; completions of other ids are stale */
  uint32_t deadline_ms;     /* chunk timeout */
  uint32_t progress_ms;     /* last send, completion or failure */
  uint32_t defer_max_ms;    /* 0: never defer */
  bool deferring;           /* a message waits for a better link since defer_since_ms */
  uint32_t defer_since_ms;
  bool no_check; /* this session needs no link check: it is urgent, or it passed one */
  bool recheck;  /* us_run() in the idle state starts the queue again (a deferral or a link check)
                  */
  struct us_stats stats;
};

void us_init(struct uplink_sched *s, uint8_t *qbuf, size_t qcap, uint8_t *abuf, size_t acap, const struct us_ops *ops,
             void *ctx);

void us_set_config(struct uplink_sched *s, sensei_Radio radio, uint8_t retries);

/** Longest time data may be held back for a bad link, ms (0: never; the radio's link check is not
 * asked). */
void us_set_defer(struct uplink_sched *s, uint32_t max_ms);

/**
 * May a data message wait for a better link? No for urgent ones (alerts, status, or a session that
 * already runs), no if deferring is off, no if the data queue is at least half full, no when the
 * wait has lasted max_ms
 * (`deferring` and since_ms say when it began). Pure.
 */
bool us_defer_ok(bool urgent, uint32_t max_ms, bool deferring, uint32_t since_ms, uint32_t now_ms, size_t q_used,
                 size_t q_cap);

/** Register a radio (static storage). -ENOSPC if the table is full. */
int us_register(struct uplink_sched *s, const struct uplink_radio *r);

/** The radio the settings select, else BLE if registered, else NULL. */
const struct uplink_radio *us_select(const struct uplink_sched *s);

/**
 * Queue an encoded Uplink. alert: alerts and status, they have their own queue and go first.
 * If the queue is full its oldest messages are dropped (counted, US_ERR_QUEUE_FULL).
 * -EINVAL if too long, -ENOMEM if it cannot fit even in an empty queue.
 */
int us_submit(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert);

/**
 * Like us_submit() for a plain (not sealed) message. When it goes out on a radio with node_id_prefix, the scheduler
 * calls ops->prepare then (the node adds Uplink.node_id), so the radio that sends it decides, not the radio at submit
 * time.
 */
int us_submit_plain(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert);

/**
 * Like us_submit() with MQ_FLAG_* bits (MQ_FLAG_PLAIN: as us_submit_plain(); MQ_FLAG_REPORT: the
 * app is told about the end of this message through ops->msg_end).
 */
int us_submit_flags(struct uplink_sched *s, const uint8_t *enc, size_t len, bool alert, uint8_t flags);

/** True if the radio that the settings select can send now. */
bool us_radio_ready(const struct uplink_sched *s);

/** Start sending the queue if idle. */
void us_start(struct uplink_sched *s);

/** Timer callback: the backoff is over, or the chunk timeout may have passed. */
void us_run(struct uplink_sched *s);

/** Messages and bytes queued in both queues. */
void us_queued(const struct uplink_sched *s, size_t *msgs, size_t *bytes);

/** True if a session has made no progress for stall_ms (for the watchdog). */
bool us_stalled(const struct uplink_sched *s, uint32_t now_ms, uint32_t stall_ms);

/** Retry delay for the attempt (0 is the first retry) with up to 50 % added by rnd. */
uint32_t us_retry_delay_ms(uint8_t attempt, uint32_t rnd);

/** Drop the session in progress (settings or radio changed). Queue stays. */
void us_abort(struct uplink_sched *s);

/** Offset in s (0 to period_s - 1) of this node's uplink time, from a hash of its id. */
uint32_t us_node_offset_s(uint64_t node_id, uint32_t period_s);

/**
 * Delay in ms to the next time that is offset_s past a multiple of period_s. Aligned to Unix time
 * if unix_valid, else to uptime (no offset: without time the nodes are not in step anyway).
 * Returns 0 if period_s is 0 (timer off).
 */
uint32_t us_next_tick_ms(bool unix_valid, uint32_t unix_s, int64_t uptime_ms, uint32_t period_s, uint32_t offset_s);

#endif /* SENSEI_UPLINK_SCHED_H_ */
