/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan.h
 * @brief Client of the LPWAN shield's nRF9151 on the nRF5340 (Zephyr): SPI master with event, busy
 *        and ack lines, shield power, LTE-M, NB-IoT and LoRaWAN. The app gives it a configuration
 *        and a few hooks; it brings the radios into its own uplink layer with sensei_lpwan_radio_ready()
 *        and sensei_lpwan_radio_send().
 *
 * One thread (`lpwan`) owns the link, the I/O expander and the state. Other contexts (the app, the
 * shell) post commands to it. Results go back through the hooks, on the hook work queue.
 * Needs the LPWAN_Shield_v1 shield (devicetree alias lpwan-link). The nRF9151 side is the app
 * NRF/apps/lpwan_shield. Protocol: lpwan_link.h.
 */

#ifndef SENSEI_LPWAN_H_
#define SENSEI_LPWAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "sensei/lpwan.pb.h"
#include "sensei_lpwan/lpwan_rai.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Payload limits of one send, in bytes: the largest LoRa payload (EU868 DR4 and up) and an LTE UDP datagram.
 *  The LoRa limit falls with the data rate: use sensei_lpwan_lora_mtu() to chunk. */
#define SENSEI_LPWAN_MTU_LORA 222u
#define SENSEI_LPWAN_MTU_LTE 1024u

/**
 * What the radios need. Takes effect at the next connect or send. APN, eDRX cycle and window are
 * Kconfig options (CONFIG_SENSEI_LPWAN_LTE_APN, _EDRX_CYCLE_MS, _EDRX_PTW_MS).
 */
struct sensei_lpwan_config {
  /* LTE-M and NB-IoT: UDP server */
  char lte_server[65]; /* host name or address */
  uint16_t lte_port;
  uint32_t lte_psm_tau_s;    /* requested PSM periodic TAU in s, 0: PSM off */
  uint32_t lte_psm_active_s; /* requested PSM active time in s */
  bool lte_edrx;             /* request eDRX (cycle and window from Kconfig) */
  uint8_t lte_rai; /* sensei_LteRai: RAI on the last chunk of a message (LTE_RAI_ONE_RESP: the server answers) */
  /* LoRaWAN (EU868, class A, OTAA) */
  uint8_t lora_dr; /* data rate 0 ... 15 */
  bool lora_adr;
  bool lora_confirmed;
  uint8_t lora_port; /* application port, 1 ... 223 */
};

/** Hooks into the app. All optional. They run on `wq`, except on_error, which may run on any thread. */
struct sensei_lpwan_hooks {
  /** Work queue for the callbacks. NULL: the system work queue. */
  struct k_work_q *wq;
  /** LoRaWAN joined or LTE connected: the uplink queue may send now. Also called (radio RADIO_BLE) when an update of
   * the shield ends and the link is free again. */
  void (*on_up)(sensei_Radio radio);
  /** A downlink arrived (an encoded app message, not decoded here). data is valid during the call only. */
  void (*on_downlink)(const uint8_t *data, size_t len, sensei_Radio radio);
  /** Network time (Unix s) from LTE or the LoRaWAN MAC. */
  void (*on_time)(uint32_t unix_s, sensei_Radio radio);
  /** Something failed (radio error, dropped downlink, send failure). Count it. */
  void (*on_error)(void);
};

/**
 * Set up the link and the shield pins, start the thread. The shield stays off until it is needed.
 * hooks and cfg are copied. Returns 0 or a negative errno (-ENODEV: shield not in the build).
 */
int sensei_lpwan_init(const struct sensei_lpwan_hooks *hooks, const struct sensei_lpwan_config *cfg);

/** Replace the configuration. Safe from any thread. */
void sensei_lpwan_set_config(const struct sensei_lpwan_config *cfg);

/**
 * True if the radio can send now (connected, joined, registered). If not, wakes the connection and
 * calls hooks.on_up when it is up. radio: RADIO_LORA, RADIO_LTEM or RADIO_NBIOT.
 */
bool sensei_lpwan_radio_ready(sensei_Radio radio);

/**
 * Bytes per LoRa chunk now (the app's fragment header included). From the configured data rate (EU868, no MAC
 * commands in the uplink): DR0 to 2: 51 B, DR3: 115 B, DR4 and up: 222 B; with ADR the network may lower the rate, so
 * DR0: 51 B. After every send the nRF9151 reports what the LR1121 takes in the next uplink; if that is less (MAC
 * answers are pending), the value is that minus 15 B. Read it when a message starts and use that size for all its
 * chunks. If done() reports -EMSGSIZE, a chunk did not fit the next uplink: read it again and chunk the message again.
 */
uint32_t sensei_lpwan_lora_mtu(void);

/**
 * Send one chunk (at most the MTU of the radio). done(err, user) runs on hooks.wq when it is out or
 * failed. Returns -EBUSY while a send is in flight, -EINVAL for a bad length, -EMSGSIZE for a LoRa chunk above the
 * nominal size of the data rate now (it was lowered after the message was chunked), -EAGAIN while LoRa sends are
 * refused for the duty cycle (see below).
 *
 * done(-EMSGSIZE): the chunk does not fit the next LoRa uplink. Read sensei_lpwan_lora_mtu() again; if it is smaller
 * than the chunk, chunk the message again. done(-EAGAIN): the LR1121 would hold the frame back for longer than a send
 * may take (10 min), so the nRF9151 refused it before queueing. Do not count it as a failure: keep the message.
 * on_up(RADIO_LORA) comes when a send may be tried again (sensei_lpwan_lora_retry_after_ms() says when);
 * sensei_lpwan_radio_ready() is false until then.
 *
 * A LoRa chunk can take minutes to go out: the LR1121 holds it back for the duty cycle (277 s after a full chunk at
 * DR0), and repeats it NbTrans times (and once more if confirmed). The nRF9151 estimates the time; done() comes when
 * it reports the result, or after the estimate plus a margin. If the LR1121 still holds the frame then, the client
 * waits once more (and the nRF9151 resets the LR1121 after twice the estimate), so the caller must not time the chunk
 * out earlier than LPWAN_SEND_MAX_MS plus a minute. The shield stays on meanwhile. A chunk that is sent again while
 * the LR1121 still holds it is not sent twice: the client waits for the first one's result.
 */
int sensei_lpwan_radio_send(sensei_Radio radio, const uint8_t *chunk, size_t len, void (*done)(int err, void *user),
                            void *user);

/**
 * Like sensei_lpwan_radio_send() and says whether the chunk is the last of its message. For LTE the last chunk gets the
 * RAI flag of the configuration (`lte_rai`): with LTE_RAI_ONE_RESP the shield waits for the answer of the server (the
 * collector's ack or a downlink; 10 s on LTE-M, 30 s on NB-IoT, from send()). If none comes the chunk was still sent:
 * done() reports 0 and sensei_lpwan_send_unconfirmed() is true inside the callback ("sent, unconfirmed"). The caller
 * must not resend the chunk at once and must not treat the message as delivered. Other chunks and the other radios
 * are as without the flag.
 *
 * Acks are best effort for plain nodes: they are not authenticated, so a forged 3-byte ack (or any datagram) that
 * arrives during the wait counts as the answer.
 */
int sensei_lpwan_radio_send_ex(sensei_Radio radio, const uint8_t *chunk, size_t len, bool last,
                               void (*done)(int err, void *user), void *user);

/**
 * True if the send whose done() runs now (call it inside the callback) went out but got no answer within the wait
 * (LTE_RAI_ONE_RESP only; see sensei_lpwan_radio_send_ex()). Counted in the shield status (lte_no_answer) and in
 * `sensei lpwan status`. False for every other send and for a send that failed.
 */
bool sensei_lpwan_send_unconfirmed(void);

/** What to do before a data uplink on LTE (from the connection evaluation, %CONEVAL). */
enum sensei_lpwan_link {
  SENSEI_LPWAN_LINK_OK,   /* send: the link is fine, or the evaluation is off or gave nothing */
  SENSEI_LPWAN_LINK_BAD,  /* the energy estimate is bad: a caller that can wait may defer */
  SENSEI_LPWAN_LINK_WAIT, /* the shield evaluates: on_up follows, ask again then */
};

/**
 * Check the LTE link before a data uplink (not for alerts and status). The first call after the last result expired
 * (`SENSEI_LPWAN_EVAL_VALID_S`) asks the shield and returns WAIT; the result is an event, on_up(LTE) follows and the
 * next call answers from it. A shield without the evaluation, or one that gives no result, is not asked again for
 * `SENSEI_LPWAN_EVAL_RETRY_S` (an hour if the shield says the evaluation is unsupported): OK meanwhile. A request whose
 * answer does not come within 30 s counts as a failed evaluation too. Call on the hook work queue (the app's).
 */
enum sensei_lpwan_link sensei_lpwan_lte_link_check(void);

/** The last evaluation of the LTE link, for the Status of the node and the shell. */
struct sensei_lpwan_lte_info {
  sensei_EnergyEstimate energy;
  bool rsrp_valid; /* false: the modem gave no valid RSRP; rsrp_dbm is 0 then and means nothing */
  int16_t rsrp_dbm;
  uint8_t ce_level; /* 0 ... 3 */
  sensei_RaiSupport rai;
  uint32_t age_s; /* since the result came */
};

/** 0, or -ENODATA if no evaluation gave a result yet. */
int sensei_lpwan_lte_info(struct sensei_lpwan_lte_info *out);

/** Time until LoRa sends are accepted again after a refusal for the duty cycle, ms; 0: now. */
uint32_t sensei_lpwan_lora_retry_after_ms(void);

/**
 * Provision LoRaWAN keys (from the shell or the host link); never stored in git. The keys are
 * sent to the shield, joined, and then dropped from RAM: the LR1121 keeps them in its own flash,
 * so the nRF5340 stores nothing. They are wiped after the shield took them, or after three failed
 * sends (provision again). Returns 0 if the request was accepted (the result is logged and visible
 * in `sensei lpwan status`), or a negative errno. The shell command echoes the keys into the shell
 * history: bench use only.
 */
int sensei_lpwan_lora_provision(const sensei_LoraProvision *keys);

/**
 * Query the nRF9151 firmware version, blocking up to timeout_ms. Powers the shield for the query.
 * Not for the work queue of the hooks. Returns -EPROTONOSUPPORT if the shield speaks another
 * protocol version (the client then refuses to use it for an hour, then asks again).
 */
int sensei_lpwan_query_version(sensei_LpwanVersion *out, uint32_t timeout_ms);

/*
 * SPI_A (P0.08 to P0.11) is shared with GAP9 (see lib/sensei_busarb). The client takes the bus for each
 * exchange with the shield; this is internal. While GAP9 holds the bus the level shifter of the shield is
 * off.
 */

/**
 * Update of the shield (the nRF9151 app, or its modem firmware). The host sends the image to the nRF5340, which relays
 * it piece by piece to the nRF9151 and keeps none of it. The messages are the dfu_* requests of lpwan.proto; the rules
 * and the flow are in sensei_lpwan/lpwan_dfu.h. What the client adds:
 *
 * - dfu_begin opens a session that holds the link: no data uplink is sent and the shield is not powered down
 *   (sensei_lpwan_radio_ready() is false, sensei_lpwan_radio_send() gives -EAGAIN, the app waits for on_up), GAP9 is
 *   kept off the shared bus (BUS_DFU), and an active radio is released first. The session ends with dfu_abort, after
 *   the new app was started, when no transfer is open any more, after 120 s without a request (the shield keeps the
 *   transfer: begin with `resume` goes on), or 45 min after it began (then no new session for 10 min). on_up (radio
 *   RADIO_BLE) tells the app that data may go again.
 * - A request that gets no answer fails with an error code; ask dfu_status for the offset and continue there.
 * - dfu_apply of the app: the shield marks its second slot for a test boot, then the client cuts the shield power,
 *   waits 300 ms, switches it on and waits up to 180 s for the new image (MCUboot swaps first; the bus lease is renewed
 *   meanwhile). If the answer to dfu_apply is lost the client asks dfu_status and goes on if the image is marked. The
 *   result carries the new version. The new image runs on trial. The client accepts it (protocol, update support kept,
 *   dfu_status answers) and sends dfu_confirm with its image id, again at every look (every 5 s) until `trial` is
 *   false, and keeps the shield powered meanwhile; an image that is not accepted is not confirmed and MCUboot goes back
 *   to the old one at the next power-up.
 * - Whenever a swap may be running (first bring-up after the node started, after an apply, after a power cut while an
 *   image was on trial, after a bring-up that timed out) the client waits for the shield up to 180 s without cutting
 *   the power. The rules and times are in sensei_lpwan/lpwan_upd.h (host-tested).
 * - Modem: dfu_apply only starts the update (minutes). The client keeps the session and the power and asks the shield
 *   itself every 5 s until the state is DFU_STATE_DONE or FAILED; the host may ask too, or be gone.
 */
struct sensei_lpwan_dfu_result {
  sensei_LpwanResult result; /* the shield's verdict (LPWAN_OK: accepted); the status tells why if it is not */
  sensei_DfuStatus status;
  bool has_version;
  sensei_LpwanVersion version; /* with dfu_status, dfu_confirm and (app) dfu_apply, once the new image answers */
};

/** done(err, res, user) runs on hooks.wq. err 0: the shield answered (see res->result); -EBUSY: another request is in
 *  flight, a send or connect is running, GAP9 holds the bus, or the session was too long (a pause follows); -ENOTSUP:
 * the shield app cannot be updated (old firmware) or lacks the target; -EPROTONOSUPPORT: it speaks another protocol;
 * -ETIMEDOUT, -EIO: the link failed. */
typedef void (*sensei_lpwan_dfu_done_fn)(int err, const struct sensei_lpwan_dfu_result *res, void *user);

/** Relay one dfu_* request (dfu_begin ... dfu_confirm of LpwanRequest). The request is copied. Returns 0 if it was
 *  queued, -EINVAL for another request, -EBUSY while one is in flight. */
int sensei_lpwan_dfu_submit(const sensei_LpwanRequest *req, sensei_lpwan_dfu_done_fn done, void *user);

/** True while an update session holds the link. */
bool sensei_lpwan_dfu_active(void);

/** The version the shield reported at the last handshake (the node puts it in Status). -ENODATA: not read yet. */
int sensei_lpwan_shield_version(sensei_LpwanVersion *out);

/** Diagnostics of the shield, read with its status after each version read, after a restart that the shield reported
 *  and before the client cuts the power of a shield that stopped answering. The shield keeps its fault record until the
 *  client has it (it acks), so one lost answer does not lose it. */
struct sensei_lpwan_shield_diag {
  uint32_t reset_cause; /**< SENSEI_RESET_* bits of the shield's last start, 0: an older shield */
  uint32_t boot_count;  /**< boots of the shield since its retained RAM was lost */
  bool has_fault;       /**< the shield reported a fatal error of its previous run (kept until the node restarts) */
  sensei_FaultRecord fault;
};

/** The shield's diagnostics (the node puts the reset cause in Status). -ENODATA: not read yet. */
int sensei_lpwan_shield_diag(struct sensei_lpwan_shield_diag *out);

/** One-line state for the shell and status messages. */
const char *sensei_lpwan_state_str(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_H_ */
