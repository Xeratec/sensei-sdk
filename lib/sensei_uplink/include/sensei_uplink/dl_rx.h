/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file dl_rx.h
 * @brief Receive side of a radio downlink: the datagrams of the collector and of TTN are chunks
 *        (msg_id | index | count | data, sensei_link/frag.h), also when the message fits one chunk.
 *        This joins them into the message that the app decodes. No Zephyr.
 *
 * One receiver per radio. The app gives the buffer; a message larger than it is refused. A 3-byte
 * acknowledgement (the collector's answer to an uplink chunk) is not a downlink and is dropped here:
 * an old shield passes it on, a new one drops it itself.
 */

#ifndef SENSEI_UPLINK_DL_RX_H_
#define SENSEI_UPLINK_DL_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_link/frag.h"

struct dl_rx {
  sensei_reasm_t reasm;
  uint32_t n_acks;    /* acknowledgements dropped */
  uint32_t n_bad;     /* chunks refused (bad header, wrong size, too large) */
  uint32_t n_message; /* messages completed */
};

/** buf, cap: the largest message (the node: a Downlink at its generated maximum inside the largest envelope). mtu:
 * largest chunk of the sender (LoRa 222, LTE 1024; LoRa chunks may be smaller, see sensei_link/frag.h). timeout_ms:
 * time without a chunk after which a message in progress is dropped. */
void dl_rx_init(struct dl_rx *rx, uint8_t *buf, size_t cap, size_t mtu, uint32_t timeout_ms);

/** Change the times at run time (LoRa: the next chunk of a message comes one uplink interval later). timeout_ms: time
 * without a chunk after which a message in progress is dropped. remember_ms: how long a finished message is remembered,
 * so that a copy of it is not applied twice (starts equal to timeout_ms). */
void dl_rx_set_timeouts(struct dl_rx *rx, uint32_t timeout_ms, uint32_t remember_ms);

/**
 * Feed one datagram received at now_ms. True when it completes a message: *msg and *len give it
 * (valid until the next call). A duplicate, an acknowledgement and a bad datagram give false.
 */
bool dl_rx_feed(struct dl_rx *rx, const uint8_t *data, size_t len, uint32_t now_ms, const uint8_t **msg,
                size_t *msg_len);

#endif /* SENSEI_UPLINK_DL_RX_H_ */
