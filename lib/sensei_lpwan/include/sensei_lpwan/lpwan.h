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
  /** LoRaWAN joined or LTE connected: the uplink queue may send now. */
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
 * failed (-EMSGSIZE: it does not fit the next LoRa uplink). Returns -EBUSY while a send is in flight, -EINVAL for
 * a bad length. A LoRa chunk can take minutes to go out: the LR1121 holds it back for the duty cycle (277 s after a
 * full chunk at DR0). done() comes when the nRF9151 reports it or after the time it announced (at most 10 min and
 * a few seconds), so the caller must not time the chunk out earlier. The shield stays on meanwhile.
 */
int sensei_lpwan_radio_send(sensei_Radio radio, const uint8_t *chunk, size_t len, void (*done)(int err, void *user),
                            void *user);

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

/** One-line state for the shell and status messages. */
const char *sensei_lpwan_state_str(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_H_ */
