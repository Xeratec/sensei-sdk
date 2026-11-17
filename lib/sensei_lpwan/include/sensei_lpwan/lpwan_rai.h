/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_rai.h
 * @brief LTE uplink policy of the shield: release assistance indication (RAI) per send, the answer of the server,
 *        the connection evaluation (%CONEVAL) classes. Pure functions and a send sequence behind an ops table: no
 *        Zephyr, no modem library, so they run on the host.
 *
 * RAI tells the modem that the uplink is over, so that it leaves the connected state at once and not after the
 * network's inactivity timer (seconds, at 70 to 130 mW in the SENSEI campaign traces). The node marks the last chunk of
 * an LTE message; the shield sets the socket option RAI before that send:
 *
 * - LTE_RAI_ONE_RESP: the server answers with one datagram (the collector always does: a downlink or a 3-byte ack).
 *   Send, wait for the answer (short wait), then RAI_NO_DATA: nothing more is expected in either direction. The send
 *   is ok only if the answer came.
 * - LTE_RAI_LAST: nothing follows the chunk. Send; the send is ok when it went out.
 * - LTE_RAI_OFF (and every chunk that is not the last one): no RAI, the send is ok when it went out.
 *
 * The options have the values of RAI_NO_DATA, RAI_LAST and RAI_ONE_RESP of Zephyr and of the modem library.
 */

#ifndef SENSEI_LPWAN_RAI_H_
#define SENSEI_LPWAN_RAI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Values of the socket option SO_RAI (RAI_NO_DATA, RAI_LAST, RAI_ONE_RESP). */
enum lpwan_rai_opt {
  LPWAN_RAI_OPT_NONE = 0,
  LPWAN_RAI_OPT_NO_DATA = 1,
  LPWAN_RAI_OPT_LAST = 2,
  LPWAN_RAI_OPT_ONE_RESP = 3,
};

/**
 * The flag a chunk gets in Send.rai: the setting `mode` (what the node wants for a message) for the last chunk,
 * LTE_RAI_OFF for every other one. Only LTE uses it.
 */
sensei_LteRai lpwan_rai_for_chunk(sensei_LteRai mode, bool last_chunk);

/** What the shield does around the send of a chunk with the flag. */
struct lpwan_rai_plan {
  enum lpwan_rai_opt before; /* SO_RAI before the send, LPWAN_RAI_OPT_NONE: none */
  bool wait_answer;          /* wait for the answer of the server after the send */
  enum lpwan_rai_opt after;  /* SO_RAI after the answer or the end of the wait, LPWAN_RAI_OPT_NONE: none */
};

struct lpwan_rai_plan lpwan_rai_plan_for(sensei_LteRai flag);

/** The socket, the send and the wait of the shield behind three calls (the host tests use fakes). */
struct lpwan_rai_io {
  /** setsockopt(SO_RAI, opt): 0, or a negative errno (the modem or the network may not support it). */
  int (*set_rai)(void *ctx, enum lpwan_rai_opt opt);
  /** One datagram: 0, or a negative errno. */
  int (*send)(void *ctx, const uint8_t *data, size_t len);
  /** Wait up to timeout_ms for the answer of the server (it is told to the shield by the receive path):
   *  true if it came. */
  bool (*wait_answer)(void *ctx, uint32_t timeout_ms);
};

struct lpwan_rai_out {
  bool sent;           /* the datagram went out */
  bool answered;       /* the answer came (only asked for with LTE_RAI_ONE_RESP) */
  unsigned rai_errors; /* set_rai calls that failed (never fail the send) */
};

/**
 * Run the sequence for one send attempt: RAI before, the send, the wait and RAI after, as the plan says. A RAI option
 * that fails does not stop the send. If the send fails there is no wait, but the option after still goes out, so the
 * modem does not keep waiting for an answer. wait_ms 0: do not wait.
 */
void lpwan_rai_send(const struct lpwan_rai_io *io, void *ctx, sensei_LteRai flag, const uint8_t *data, size_t len,
                    uint32_t wait_ms, struct lpwan_rai_out *out);

/** The send is ok: it went out and, if an answer was waited for, it came. */
bool lpwan_rai_send_ok(sensei_LteRai flag, uint32_t wait_ms, const struct lpwan_rai_out *out);

/** A datagram the shield received after a send. */
enum lpwan_answer {
  LPWAN_ANSWER_ACK,      /* the ack of the chunk that was sent: an answer, not for the node */
  LPWAN_ANSWER_OLD_ACK,  /* an ack of another chunk (late): drop it */
  LPWAN_ANSWER_DOWNLINK, /* anything else: a downlink for the node, and an answer */
};

/**
 * Classify a datagram. chunk_msg_id and chunk_index are the first two bytes of the chunk the shield sent last
 * (the fragment header); has_chunk false: no send waits (every ack is old).
 */
enum lpwan_answer lpwan_answer_classify(const uint8_t *data, size_t len, bool has_chunk, uint8_t chunk_msg_id,
                                        uint8_t chunk_index);

/** Energy estimate of %CONEVAL (5 ... 9) as the enum of the link; ENERGY_UNKNOWN for anything else. */
sensei_EnergyEstimate lpwan_energy_from_modem(int modem_value);

/** Bad conditions: the estimate says excessive. Unknown is not bad (nothing is deferred without a result). */
bool lpwan_energy_is_bad(sensei_EnergyEstimate e);

/**
 * What the client does with the connection evaluation before a send that can wait. It keeps the last result for
 * `valid_ms`, asks the shield when it has none, waits `timeout_ms` for the answer, and after a failed evaluation (no
 * result, an older shield) does not ask again for `retry_ms`: then the send goes ahead. Times are uptime in ms (wrap
 * safe).
 */
struct lpwan_eval_state {
  bool have;      /* a result is stored */
  uint32_t at;    /* when it came */
  bool bad;       /* it said bad */
  bool pending;   /* the shield was asked, no event yet */
  uint32_t asked; /* when */
  bool failed;    /* the last evaluation gave nothing */
  uint32_t failed_at;
};

enum lpwan_eval_action {
  LPWAN_EVAL_OK,   /* send now: the result is good, or there is none and it is not asked for */
  LPWAN_EVAL_BAD,  /* the stored result says bad */
  LPWAN_EVAL_WAIT, /* asked, the event comes: try again then */
  LPWAN_EVAL_ASK,  /* ask the shield now (the caller sets pending and asked) */
};

enum lpwan_eval_action lpwan_eval_decide(const struct lpwan_eval_state *s, uint32_t now, uint32_t valid_ms,
                                         uint32_t timeout_ms, uint32_t retry_ms);

/** Store the answer: valid false (or a failed request) counts as a failed evaluation. */
void lpwan_eval_result(struct lpwan_eval_state *s, uint32_t now, bool valid, bool bad);

/** RSRP in dBm from the index of the modem (-17 ... 97, 0 is not used): (index - 140) below 0, else (index - 141). */
int lpwan_rsrp_dbm(int idx);

/** RSRQ in whole dB (rounded towards zero) from the index of the modem: (index - 39) / 2 below 0, (index - 40) / 2 up
 * to 34, (index - 41) / 2 above. */
int lpwan_rsrq_db(int idx);

/** RAI support of the cell from the %RAI report. */
sensei_RaiSupport lpwan_rai_support(bool as_rai, bool cp_rai);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_RAI_H_ */
