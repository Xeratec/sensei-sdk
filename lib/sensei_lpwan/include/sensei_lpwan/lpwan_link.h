/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * The transfer scheme is adapted from the LPWAN shield commissioning project (ETH Zurich, 2026).
 */

/**
 * @file lpwan_link.h
 * @brief Constants of the SPI link between the nRF5340 (master) and the nRF9151 (slave) of the
 *        LPWAN shield, and the global state machine that both sides run.
 *
 * Shared by the nRF9151 app and the nRF5340 client. No Zephyr dependency.
 *
 * Transfer scheme
 * ---------------
 * - Every transfer is full duplex and has the fixed length LPWAN_XFER_LEN. Each side sends exactly
 *   one SPI frame (sensei_spi header + payload, rest padding) or an idle frame.
 * - The slave arms its buffers, then drives BUSY low. The master may start a transfer only while
 *   BUSY is low. The slave drives BUSY high as soon as the transfer ends and while it processes.
 * - The master sends a LpwanRequest (type 0x20). The slave answers in the next transfer with the
 *   LpwanResponse (0x21) of the same seq. Slow work (connect, join, send) is reported by
 *   LpwanEvent frames (0x22) later.
 * - The slave raises EVENT while it holds events. The master then runs transfers with idle frames
 *   until EVENT is low again.
 * - A frame that arrives with a bad magic, length or CRC (the CRC covers the header too) is dropped.
 *   The slave keeps every message (response or event) armed until the master confirms it: the
 *   master sets LPWAN_FLAG_ACK and copies the toggle bit of that message into LPWAN_FLAG_ACK_TOGGLE.
 *   The slave releases its armed message only if the toggle bit matches, so a late or repeated
 *   ACK never releases the next message. A lost or damaged message is sent again. The master drops
 *   repeats with the toggle bit (the first message after a start is always taken). A request that
 *   is repeated with the same seq is not executed twice: the slave returns the stored response.
 * - When the shield restarts the master calls lpwan_master_reset(). When the master gives up (the
 *   link fails several times in a row) it power-cycles the shield.
 */

#ifndef SENSEI_LPWAN_LINK_H_
#define SENSEI_LPWAN_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"
#include "sensei_link/sensei_link.h"
#include "sensei_link/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version of the LPWAN protocol (messages and their meaning), reported in LpwanVersion.protocol.
 *  The client refuses a shield with another value (-EPROTONOSUPPORT). Bump it with a change of
 *  lpwan.proto or of the transfer rules that an older peer would misread. */
#define LPWAN_PROTOCOL_VERSION 2u

/** Bytes per transfer: header and the largest message (LpwanRequest 1046 B), rounded up. */
#define LPWAN_XFER_LEN 1056u
#define LPWAN_MAX_PAYLOAD (LPWAN_XFER_LEN - SENSEI_SPI_HEADER_SIZE)

/** Frame type of an idle frame (no message). */
#define LPWAN_FRAME_IDLE 0x00u

/*
 * Link flags in the SPI frame header (in addition to SENSEI_SPI_FLAG_ERROR 0x01 and _MORE 0x02):
 * - LPWAN_FLAG_ACK: the master took a message of the slave. LPWAN_FLAG_ACK_TOGGLE is the toggle
 *   bit of that message.
 * - LPWAN_FLAG_TOGGLE: set by the slave on its messages, flipped for every new message (alternating
 *   bit). The master drops a message whose bit equals that of the last one it took (a repeat).
 */
#define LPWAN_FLAG_ACK 0x04u
#define LPWAN_FLAG_TOGGLE 0x08u
#define LPWAN_FLAG_ACK_TOGGLE 0x10u

/* ---- Global state machine (both MCUs) ---- */

typedef enum {
  LPWAN_ST_IDLE,
  LPWAN_ST_CONNECTING,
  LPWAN_ST_CONNECTED,
  LPWAN_ST_SENDING,
  LPWAN_ST_DISCONNECTING,
  LPWAN_ST_ERROR,
} lpwan_state_t;

/** Only one technology is active at a time, so the radios do not disturb each other. */
typedef enum {
  LPWAN_TECH_NONE,
  LPWAN_TECH_LTE,
  LPWAN_TECH_LORA,
  LPWAN_TECH_BLE,
} lpwan_tech_t;

typedef struct {
  lpwan_state_t state;
  lpwan_tech_t tech;
  uint32_t send_seq; /* seq of the send in flight (0: none): only its tx_done ends SENDING */
} lpwan_sm_t;

void lpwan_sm_init(lpwan_sm_t *sm);

/**
 * Check a request against the state and apply the transition it starts.
 * Returns LPWAN_OK or the result code to send back. A rejected request changes nothing.
 * `send` records req->seq as the send in flight: the caller has set it before (the master side with
 * lpwan_master_next_seq()), so a tx_done that arrives while the request is still on the wire matches.
 *
 *   get_version, get_status             any state
 *   lte_configure, lora_configure,
 *   lora_provision                      IDLE
 *   lte_connect, lora_join              IDLE, tech NONE, then CONNECTING (tech LTE or LORA)
 *   send                                CONNECTED with the matching tech, then SENDING
 *   get_time                            CONNECTED
 *   lte_disconnect                      CONNECTING, CONNECTED, ERROR or IDLE: the active
 *                                       technology is released (LTE detaches, LoRa is parked)
 *   sleep                               IDLE or CONNECTED
 *   dfu_begin, _chunk, _finish, _apply,
 *   _abort                              IDLE (the update holds the link: no radio is active)
 *   dfu_status, dfu_confirm             any state
 */
sensei_LpwanResult lpwan_sm_admit(lpwan_sm_t *sm, const sensei_LpwanRequest *req);

/**
 * Apply an event: connected/joined, disconnected, tx_done, error. Returns false if the event does
 * not fit the machine and was ignored: the radio is not the active technology (a late LoRa join
 * during an LTE connect), the machine is not waiting for it, or a tx_done of another send (or a second one
 * for a send that ended). An error event does not end a send: its tx_done (or the timeout) does.
 * Events that carry data only (downlink, time) return true and change nothing.
 */
bool lpwan_sm_event(lpwan_sm_t *sm, const sensei_LpwanEvent *evt);

/** True for events that move the state machine (joined, connected, disconnected, tx_done, error).
 *  The others (downlink, network time) may be dropped when the event queue is nearly full. */
bool lpwan_event_is_state(const sensei_LpwanEvent *evt);

/** The master gives up the send in flight (timeout): SENDING becomes CONNECTED. */
void lpwan_sm_abort_send(lpwan_sm_t *sm);

/**
 * A request failed after lpwan_sm_admit() had moved the machine from `before` to `admitted`: go
 * back to `before`, unless events that arrived meanwhile have moved it on.
 */
void lpwan_sm_revert(lpwan_sm_t *sm, const lpwan_sm_t *before, const lpwan_sm_t *admitted);

/** Technology of a radio id; LPWAN_TECH_NONE for BLE-less ids that are not an LPWAN radio. */
lpwan_tech_t lpwan_tech_of_radio(sensei_Radio r);

/** The BLE side claims or releases the radio time (nRF5340 only). Fails if another one is active.
 */
sensei_LpwanResult lpwan_sm_claim_ble(lpwan_sm_t *sm, int claim);

const char *lpwan_state_name(lpwan_state_t s);
const char *lpwan_tech_name(lpwan_tech_t t);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_LINK_H_ */
