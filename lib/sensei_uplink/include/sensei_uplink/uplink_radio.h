/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file uplink_radio.h
 * @brief What an app tells the uplink scheduler about a radio: chunk size, timeouts, how to send a chunk. The table is
 *        the app's; the scheduler (uplink_sched.h) only reads it. No Zephyr.
 */

#ifndef SENSEI_UPLINK_RADIO_H_
#define SENSEI_UPLINK_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/types.pb.h"

/** What a radio says about its link before a non-urgent message (LTE: connection evaluation). */
enum uplink_link {
  US_LINK_OK,   /* send now (also: the radio does not know) */
  US_LINK_BAD,  /* the link is bad: the scheduler may hold the data, bounded by the deferral setting */
  US_LINK_WAIT, /* the radio is finding out and calls its up hook when it knows: ask again then */
};

/** done() result of send_last(): the chunk went out, the answer of the server did not come. */
#define US_SENT_UNCONFIRMED 1

/** A radio backend (static storage; registered with us_register()). */
struct uplink_radio {
  sensei_Radio id;
  size_t mtu; /* largest chunk in bytes, including the 3 B fragment header (at most US_MAX_MTU) */
  /** True if the radio can send now (connected/joined/registered). */
  bool (*ready)(void);
  /** Send one chunk; call done(err, user) when it is out (0) or failed (below 0). done() runs in the same context as
   * the scheduler's other calls (the app work queue on the node), also when it is called from inside send(). */
  int (*send)(const uint8_t *chunk, size_t len, void (*done)(int err, void *user), void *user);
  /** Optional: the chunk size now (at most mtu), read when a message starts. NULL: mtu. */
  size_t (*mtu_now)(void);
  /** Optional: how long one chunk may take until done() (0: US_CHUNK_TIMEOUT_MS). */
  uint32_t chunk_timeout_ms;
  /** The app wants to change a plain (unsealed) message before it goes out on this radio: the scheduler calls
   * us_ops.prepare for it (the node adds Uplink.node_id on plain LTE). */
  bool prepare;
  /** The collector keeps the chunks of a partly received message while this radio pauses (-EAGAIN, -ENOTCONN). LoRa:
   * yes, TTN uplinks are joined per device for the collector's LORA_REASSEMBLY_TIMEOUT (default 700 s). LTE: no, a new
   * connection comes from a new source address and the collector starts a new message. Only a radio with this flag
   * gets a deferred message resumed at the refused chunk (and then only within US_RESUME_MAX_MS, which must stay
   * below the collector's timeout); else the message starts again with a new id. */
  bool keeps_reasm;
  /** Optional, used instead of send(): `last` says the chunk is the last of the session (the last chunk of the last
   * queued message). LTE sets the release assistance indication on it and waits for the answer of the collector:
   * done(US_SENT_UNCONFIRMED) if it does not come. Other chunks are as with send(). */
  int (*send_last)(const uint8_t *chunk, size_t len, bool last, void (*done)(int err, void *user), void *user);
  /** Optional: asked before a non-urgent message starts (never for alerts and status, never in a session that an
   * alert opened), only while the deferral setting is on. */
  enum uplink_link (*link_check)(void);
};

#endif /* SENSEI_UPLINK_RADIO_H_ */
