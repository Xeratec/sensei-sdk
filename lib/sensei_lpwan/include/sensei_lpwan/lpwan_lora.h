/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_lora.h
 * @brief LoRaWAN payload limits. Pure functions.
 */

#ifndef SENSEI_LPWAN_LORA_H_
#define SENSEI_LPWAN_LORA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Largest application payload of one uplink in EU868 (LoRaWAN RP002-1.0.4, repeater compatible, no
 * MAC commands in FOpts): DR0 to DR2 51 B, DR3 115 B, DR4 to DR7 222 B. A data rate above 7 is not an
 * EU868 rate: the smallest value is returned. With `adr` the network may move the device to any
 * rate, so the DR0 value is returned (the one that always fits).
 */
uint32_t lpwan_lora_max_payload(uint8_t data_rate, bool adr);

/** Margin when the LR1121 owes MAC answers: the network may add more before the next uplink (FOpts: 15 B). */
#define LPWAN_LORA_MAC_MARGIN 15u
/** LoRaWAN frame bytes around the application payload (MHDR, DevAddr, FCtrl, FCnt, FPort, MIC = 13) plus
 *  15 B of FOpts, for the airtime estimate. */
#define LPWAN_LORA_PHY_OVERHEAD 28u
/** Receive windows after a LoRaWAN uplink (RX1 after 1 s, RX2 after 2 s, plus the window), ms. */
#define LPWAN_LORA_RX_MS 3000u
/** Longest time a send may take before it is given up, ms. */
#define LPWAN_SEND_MAX_MS 600000u

/**
 * Bytes per chunk (the fragment header included) for the next LoRa uplink. `next_max` is what the LR1121
 * reported for it (0: not known): it counts the MAC answers the LR1121 owes. If it is below the nominal
 * value, MAC traffic is flowing: LPWAN_LORA_MAC_MARGIN more are left free. Never below 4.
 */
uint32_t lpwan_lora_chunk_max(uint8_t data_rate, bool adr, uint32_t next_max);

/**
 * True if a chunk of `len` bytes that the LR1121 refused (`next_max`: what it takes, 0: not known) is above the size
 * sensei_lpwan_lora_mtu() gives now: chunking the message again helps (-EMSGSIZE). If not, the same size would be
 * refused again (the LR1121 did not say, or the chunk is not above the size it says).
 */
bool lpwan_lora_chunk_too_big(uint8_t data_rate, bool adr, uint32_t next_max, uint32_t len);

/** Time on air of a LoRaWAN uplink of `phy_len` bytes (EU868 data rates 0 to 7; 8 and up: as DR0), ms, rounded up. */
uint32_t lpwan_lora_airtime_ms(uint8_t data_rate, uint32_t phy_len);

/** Smallest chunk the fragmenter takes (3 B header and one byte). Below this the LR1121 has no room for data. */
#define LPWAN_LORA_MIN_CHUNK 4u
/** Largest NbTrans the LR1121 accepts. */
#define LPWAN_LORA_NB_TRANS_MAX 15u

/**
 * Time from the first transmission to tx_done: `nb_trans` transmissions (1 to 15, else 1) and one more if `confirmed`
 * (a missing ack repeats the frame). Each takes the airtime (DR0 with `adr`: the network may lower the rate) and its
 * receive windows; the next starts when the duty cycle allows it (1 %: 99 times the airtime after the frame, at least
 * after the windows). Saturates at UINT32_MAX.
 */
uint32_t lpwan_lora_tx_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t nb_trans, bool confirmed);

/**
 * Transmissions of one LoRa uplink, as the shield reports them in TxDone.attempts: `nb_trans` (1 to 15, else 1) for an
 * unconfirmed one; a confirmed one counts 1 if it was acknowledged and `nb_trans` + 1 if not (the missing ack repeats
 * it, like lpwan_lora_tx_ms()). The LR1121 does not say how many frames it sent: this is the same model as the time
 * estimate, and a lower bound for an acknowledged uplink (it may have taken several tries).
 */
uint32_t lpwan_lora_tx_attempts(uint32_t nb_trans, bool confirmed, bool acked);

/** Status byte of the Modem-E TX_DONE event. */
#define LPWAN_LORA_TX_NOT_SENT 0x00    /**< unconfirmed: the frame did not go out; confirmed: no ack came */
#define LPWAN_LORA_TX_UNCONFIRMED 0x01 /**< sent, no ack asked for */
#define LPWAN_LORA_TX_CONFIRMED 0x02   /**< sent and acknowledged */

/**
 * TxDone.attempts from the status of the TX_DONE event: 0 if an unconfirmed uplink was not sent (nothing went out, so
 * nothing to count); else lpwan_lora_tx_attempts() (a confirmed uplink with status NOT_SENT had no ack: its frames
 * went out).
 */
uint32_t lpwan_lora_tx_done_attempts(uint8_t status, uint32_t nb_trans, bool confirmed);

/** What the shield read from the LR1121 for a send. A call that failed is `*_known == false`. */
struct lpwan_lora_est_in {
  uint8_t data_rate;
  bool adr;
  uint32_t app_len;
  bool confirmed;
  bool duty_known;  /* get_duty_cycle_status answered */
  int32_t duty;     /* < 0: the LR1121 holds a frame back this many ms; >= 0: no wait */
  bool nb_known;    /* get_nb_trans answered */
  uint8_t nb_trans; /* 0 or above 15: not valid */
};

struct lpwan_lora_est {
  uint32_t wait_ms; /* duty-cycle wait before the first transmission */
  uint32_t tx_ms;   /* first transmission to tx_done, see lpwan_lora_tx_ms() */
};

/**
 * The estimate of a send. If the LR1121 did not answer or gave a nonsense value, the estimate is conservative and
 * never 0: DR0, NbTrans 1, confirmed, and a wait as if a frame of the same size had just gone out.
 */
void lpwan_lora_estimate(const struct lpwan_lora_est_in *in, struct lpwan_lora_est *out);

/**
 * Time from the send request to tx_done for an application payload of `app_len` bytes: wait plus transmissions, see
 * lpwan_lora_tx_ms(). At most LPWAN_SEND_MAX_MS.
 */
uint32_t lpwan_lora_expected_ms(uint8_t data_rate, bool adr, uint32_t app_len, uint32_t duty_wait_ms, uint32_t nb_trans,
                                bool confirmed);

/**
 * A send whose duty-cycle wait would make it take longer than LPWAN_SEND_MAX_MS is refused before it is queued, and
 * the caller is told when to try again. Returns 0 if the send may be queued, else the ms until it may (the frame
 * itself always fits if it alone takes more than LPWAN_SEND_MAX_MS: then only the wait counts).
 */
uint32_t lpwan_lora_retry_after_ms(const struct lpwan_lora_est *est);

/** What the shield read from the LR1121 and from its own state, for lpwan_lora_plan_send(). */
struct lpwan_lora_send_in {
  bool joined;           /* the radio is ready (or sending) */
  bool held;             /* the LR1121 holds a frame */
  uint32_t held_seq;     /* seq of the Send it belongs to, 0: not a client frame (MAC flush) */
  uint32_t held_left_ms; /* time until its tx_done is expected */
  uint32_t len;
  bool max_known; /* get_next_tx_max_payload answered; 0 counts as not known */
  uint8_t max;
  struct lpwan_lora_est_in est; /* est.app_len is set by the planner */
};

struct lpwan_lora_send_out {
  sensei_LpwanResult result; /* OK, INVALID, BUSY, NOT_JOINED */
  uint32_t expected_ms;      /* OK: time to tx_done. BUSY: time until the frame is out or a send may be accepted */
  uint32_t next_max;         /* payload the next uplink takes, 0: not known */
  uint32_t queued_seq;       /* BUSY: seq of the Send whose frame the LR1121 holds, 0: none */
  bool flush;                /* BUSY: the shield queues an empty uplink to flush the MAC answers */
};

/**
 * Decide what a LoRa send request gets, before anything is queued:
 *   len 0 or above 222, or `joined` false:      INVALID, NOT_JOINED
 *   a frame is held:                            BUSY, queued_seq and the time left
 *   the next uplink takes less than the chunk:  INVALID, next_max (max 0 is "not known", no check)
 *   it takes less than LPWAN_LORA_MIN_CHUNK:    BUSY, flush (an empty uplink carries the MAC answers)
 *   the duty-cycle wait is too long:            BUSY, the time after which it fits
 *   else                                        OK, expected_ms
 */
void lpwan_lora_plan_send(const struct lpwan_lora_send_in *in, struct lpwan_lora_send_out *out);

/* ---- Shield side: the send that waits for its result ---- */

/**
 * The nRF9151 side of one send in flight: its seq, the watchdog, the late result. Pure; the caller holds a lock.
 * A send has one result: the first tx_done for its seq. A tx_done for another seq is late (the send was given up, or
 * it belongs to a frame the client never asked for): it is kept once for the status.
 */
struct lpwan_txtrack {
  uint32_t open_seq; /* send waiting for its result, 0: none */
  uint32_t wd_seq;   /* send the armed watchdog belongs to, 0: none */
  uint32_t start_ms;
  uint32_t limit_ms;
  uint32_t late_seq; /* result of a send that was not open, 0: none */
  bool late_ok;
};

enum lpwan_wd_action {
  LPWAN_WD_NONE,  /* nothing to do: the watchdog is for a send that has its result, or another send */
  LPWAN_WD_WAIT,  /* the LR1121 still holds the frame: look again after `*rearm_ms` */
  LPWAN_WD_FAIL,  /* no frame, no result: end the send with tx_done(failed) */
  LPWAN_WD_RESET, /* the frame is stuck for twice the limit: reset the LR1121, end the send */
};

void lpwan_txt_init(struct lpwan_txtrack *t);
/** A send request was admitted. Before the backend runs: it may answer at once. */
void lpwan_txt_open(struct lpwan_txtrack *t, uint32_t seq);
/** The backend refused the send. */
void lpwan_txt_refused(struct lpwan_txtrack *t, uint32_t seq);
/**
 * The backend took the send; the watchdog is armed for `limit_ms`. Returns false if the result is in already (do not
 * arm it).
 */
bool lpwan_txt_armed(struct lpwan_txtrack *t, uint32_t seq, uint32_t now_ms, uint32_t limit_ms);
/** The send `seq` is the one that waits for its result. */
bool lpwan_txt_wants(const struct lpwan_txtrack *t, uint32_t seq);
/** The result of `seq` was passed on. */
void lpwan_txt_done(struct lpwan_txtrack *t, uint32_t seq);
/** A tx_done that no send waits for: kept for the status (the last one wins). */
void lpwan_txt_late(struct lpwan_txtrack *t, uint32_t seq, bool ok);
/** The late result for the status, once. Returns false if there is none. */
bool lpwan_txt_take_late(struct lpwan_txtrack *t, uint32_t *seq, bool *ok);
/**
 * The watchdog fired at `now_ms`. `frame_held`: the LR1121 still holds the frame of the open send. While it does, the
 * send is not failed (it would go out later and a retry would send it twice): the watchdog looks again, until twice
 * the limit has passed. `*seq` is the send the action is for.
 */
enum lpwan_wd_action lpwan_txt_wd(const struct lpwan_txtrack *t, uint32_t now_ms, bool frame_held, uint32_t *seq,
                                  uint32_t *rearm_ms);

/* ---- Client side: the frame the shield may still hold ---- */

/** The last send the shield took and has not reported a result for. */
struct lpwan_held {
  bool held;
  uint32_t seq;
  uint32_t hash;
  uint16_t len;
  uint32_t until_ms; /* the shield resets the LR1121 by then at the latest */
};

enum lpwan_busy_action {
  LPWAN_BUSY_ADOPT,       /* the shield holds this very chunk: wait for its tx_done, that is the result */
  LPWAN_BUSY_WAIT_RESEND, /* it holds another frame: wait for its tx_done, then send this chunk */
  LPWAN_BUSY_RETRY_LATER, /* nothing is held: the shield says when it may be tried again */
};

enum lpwan_deadline_action {
  LPWAN_DL_GIVE_UP, /* the shield holds nothing and gave no result: the send failed */
  LPWAN_DL_EXTEND,  /* the shield holds the frame: wait for its end */
  LPWAN_DL_OK,      /* the status carries a late result: success */
  LPWAN_DL_FAILED,  /* the status carries a late result: failure */
};

void lpwan_held_arm(struct lpwan_held *h, uint32_t seq, const uint8_t *data, size_t len);
/** A tx_done for `seq` arrived, or the shield refused the send: the frame is not held (any more). */
void lpwan_held_result(struct lpwan_held *h, uint32_t seq);
void lpwan_held_clear(struct lpwan_held *h);
/** The shield took the send; its result is expected within `period_ms` (the shield gives up after twice that). */
void lpwan_held_expect(struct lpwan_held *h, uint32_t now_ms, uint32_t period_ms);
/** The shield may hold the frame still: the power must stay on. */
bool lpwan_held_keeps_power(const struct lpwan_held *h, uint32_t now_ms);
/** The shield answered a send with BUSY and `queued_seq` (0: none). */
enum lpwan_busy_action lpwan_held_busy(const struct lpwan_held *h, const uint8_t *data, size_t len,
                                       uint32_t queued_seq);
/**
 * The client's deadline passed for `seq`; `status_known` if the shield answered a status request, with the fields of
 * LpwanStatus. `extended` if it waited once more already.
 */
enum lpwan_deadline_action lpwan_held_deadline(uint32_t seq, bool extended, bool status_known,
                                               const sensei_LpwanStatus *st);

/**
 * Time limit of a send: `expected_ms` (0: not known) plus `extra_ms`, not below `floor_ms`. The nRF9151 uses a
 * small `extra_ms`, the nRF5340 a larger one, so the nRF9151 ends a stuck send first.
 */
uint32_t lpwan_send_limit_ms(uint32_t expected_ms, uint32_t floor_ms, uint32_t extra_ms);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_LORA_H_ */
