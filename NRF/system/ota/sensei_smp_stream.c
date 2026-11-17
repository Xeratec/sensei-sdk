/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SMP (mcumgr serial line framing) on a byte stream that also carries 0x00-delimited frames of another
 * protocol. Modelled on Zephyr's smp_uart.c and smp_dummy.c; the UART driver is replaced by the
 * feed/tx hooks of sensei_ota.h. */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/serial.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/net_buf.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

#include "ota/sensei_ota.h"

LOG_MODULE_DECLARE(sensei_ota, CONFIG_SENSEI_OTA_LOG_LEVEL);

#define LINE_MAX MCUMGR_SERIAL_MAX_FRAME

enum rx_state {
  RX_IDLE, /* at a frame boundary */
  RX_HEAD, /* held the first marker byte */
  RX_LINE, /* inside an SMP line */
  RX_MID,  /* inside a frame of the other protocol */
};

static struct smp_transport transport;
static struct mcumgr_serial_rx_ctxt rx_ctxt;
static sensei_smp_stream_tx_fn tx_fn;
static void *tx_user;

static enum rx_state state = RX_IDLE;
static uint8_t held;
static uint8_t line[LINE_MAX];
static size_t line_len;
static uint8_t tx_line[LINE_MAX + 2];
static size_t tx_len;
static volatile int64_t last_line_ms = INT64_MIN / 2;

static uint16_t get_mtu(const struct net_buf *nb) { return CONFIG_SENSEI_OTA_STREAM_MTU; }

/* mcumgr_serial_tx_pkt() hands out small pieces; a line ends with '\n'. */
static int tx_piece(const void *data, int len) {
  const uint8_t *p = data;

  for (int i = 0; i < len; i++) {
    if (tx_len >= sizeof(tx_line)) {
      tx_len = 0;
      return -EMSGSIZE;
    }
    tx_line[tx_len++] = p[i];
    if (p[i] == '\n') {
      int rc = tx_fn(tx_line, tx_len, tx_user);

      tx_len = 0;
      if (rc != 0) {
        return rc;
      }
    }
  }
  return 0;
}

static int tx_pkt(struct net_buf *nb) {
  int rc;

  tx_len = 0;
  rc = mcumgr_serial_tx_pkt(nb->data, nb->len, tx_piece);
  smp_packet_free(nb);
  return rc;
}

static void handle_line(void) {
  struct net_buf *nb = mcumgr_serial_process_frag(&rx_ctxt, line, (int)line_len);

  last_line_ms = k_uptime_get();
  if (nb != NULL) {
    smp_rx_req(&transport, nb);
  }
}

size_t sensei_smp_stream_feed(uint8_t byte, uint8_t pass[2]) {
  switch (state) {
  case RX_IDLE:
    if (byte == MCUMGR_SERIAL_HDR_PKT_1 || byte == MCUMGR_SERIAL_HDR_FRAG_1) {
      held = byte;
      state = RX_HEAD;
      return 0;
    }
    state = byte == 0 ? RX_IDLE : RX_MID;
    pass[0] = byte;
    return 1;
  case RX_HEAD:
    if ((held == MCUMGR_SERIAL_HDR_PKT_1 && byte == MCUMGR_SERIAL_HDR_PKT_2) ||
        (held == MCUMGR_SERIAL_HDR_FRAG_1 && byte == MCUMGR_SERIAL_HDR_FRAG_2)) {
      line[0] = held;
      line[1] = byte;
      line_len = 2;
      state = RX_LINE;
      return 0;
    }
    state = byte == 0 ? RX_IDLE : RX_MID;
    pass[0] = held;
    pass[1] = byte;
    return 2;
  case RX_LINE:
    if (byte == '\n') {
      handle_line();
      state = RX_IDLE;
      return 0;
    }
    if (byte == 0) { /* the other protocol's delimiter: drop the line */
      state = RX_IDLE;
      pass[0] = byte;
      return 1;
    }
    if (line_len >= sizeof(line)) { /* too long: not a line */
      state = RX_MID;
      return 0;
    }
    line[line_len++] = byte;
    return 0;
  default: /* RX_MID */
    if (byte == 0) {
      state = RX_IDLE;
    }
    pass[0] = byte;
    return 1;
  }
}

void sensei_smp_stream_reset(void) {
  state = RX_IDLE;
  line_len = 0;
}

bool sensei_smp_stream_active(uint32_t ms) { return k_uptime_get() - last_line_ms < (int64_t)ms; }

int sensei_smp_stream_init(sensei_smp_stream_tx_fn tx, void *user) {
  tx_fn = tx;
  tx_user = user;
  transport.functions.output = tx_pkt;
  transport.functions.get_mtu = get_mtu;
  return smp_transport_init(&transport);
}
