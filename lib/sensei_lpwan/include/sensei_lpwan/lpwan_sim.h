/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file lpwan_sim.h
 * @brief The link between the simulated nRF5340 (client) and the simulated nRF9151 (shield app) on native_sim.
 *
 * Instead of SPI, BUSY and EVENT the two processes talk over one TCP connection (the shield listens, the client
 * connects). The SPI frames are the same as on the board. Pure code, no Zephyr dependency, host tested.
 *
 * Stream: frames `type (1 B), length (2 B, little endian), payload`.
 *   client -> shield  LPWAN_SIM_XFER      one SPI transfer: the frame the client clocks out (LPWAN_XFER_LEN bytes)
 *   shield -> client  LPWAN_SIM_XFER_RSP  what the slave had armed, the answer to that transfer
 *   shield -> client  LPWAN_SIM_PINS      BUSY and EVENT levels (1 B each), sent when one changes and at connect
 * A transfer that finds no armed buffer (BUSY high) is answered with zeros and the client's frame is dropped.
 *
 * Power: the connection is the supply of the shield. Connecting powers it, closing the connection cuts the power: the
 * shield process restarts and listens again, so it has lost its RAM as after a power cycle.
 */

#ifndef SENSEI_LPWAN_SIM_H_
#define SENSEI_LPWAN_SIM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LPWAN_SIM_XFER 0x01u
#define LPWAN_SIM_XFER_RSP 0x02u
#define LPWAN_SIM_PINS 0x03u

#define LPWAN_SIM_HEADER 3u
#define LPWAN_SIM_MAX_PAYLOAD 2048u

/** Write a frame to `out` (LPWAN_SIM_HEADER + len bytes). Returns its size, 0 if `len` is too large. */
size_t lpwan_sim_pack(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out);

/** Receive buffer of a stream: append what the socket gave to `buf + n`, then take frames. */
struct lpwan_sim_rx {
  uint8_t buf[LPWAN_SIM_HEADER + LPWAN_SIM_MAX_PAYLOAD];
  size_t n;
};

/** Room left in the buffer (read at most this many bytes into `buf + n`). */
static inline size_t lpwan_sim_rx_room(const struct lpwan_sim_rx *r) { return sizeof(r->buf) - r->n; }

/**
 * The oldest whole frame in the buffer: 1 with `*type`, `*payload` (points into the buffer) and `*len`; 0 if it is not
 * complete yet; -1 if the length field is above the maximum (the stream is broken: close it). Call
 * lpwan_sim_rx_pop() when the frame has been used.
 */
int lpwan_sim_rx_next(const struct lpwan_sim_rx *r, uint8_t *type, const uint8_t **payload, size_t *len);

/** Drop the frame lpwan_sim_rx_next() returned. */
void lpwan_sim_rx_pop(struct lpwan_sim_rx *r);

/* ---- Host side (native_sim only, lpwan_sim_host.c). Non-blocking, host sockets, closed on exec. ---- */

/** Listen on `bind_addr:port`. Returns the fd or -1. */
int lpwan_sim_listen(const char *bind_addr, int port);
/** Accept a connection: the fd, -2 if none waits, -1 on an error. */
int lpwan_sim_accept(int listen_fd);
/** Connect to `host:port` (name or address). Returns the fd or -1. */
int lpwan_sim_connect(const char *host, int port);
/** Read: the number of bytes, 0 when the peer closed, -2 if nothing is there yet, -1 on an error. */
int lpwan_sim_read(int fd, void *buf, size_t n);
/** Write everything (waits up to 1 s of host time for a full socket). Returns 0 or -1. */
int lpwan_sim_write(int fd, const void *buf, size_t n);
void lpwan_sim_close(int fd);
/** Wall clock of the host, ms since 1970 (UTC). The shield simulator takes the network time from it. */
int64_t lpwan_sim_utc_ms(void);
/** Contents of a file, up to `cap` bytes: the size, or -1 if it cannot be read. */
long lpwan_sim_file_read(const char *path, void *buf, size_t cap);
/** Replace the contents of a file (written to a temporary file first). Returns 0 or -1. */
int lpwan_sim_file_write(const char *path, const void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LPWAN_SIM_H_ */
