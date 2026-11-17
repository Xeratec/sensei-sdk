/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_side.h
 * @brief GAP9 side of the SPI link to the nRF5340 (GAP9 is the SPI master). Built into GAP9 apps with
 *        the GAP SDK (see gap9_side.cmake). The protocol is in PROTOCOL.md.
 *
 * The link moves opaque job and result messages: the app decodes jobs and encodes results (any
 * message schema). A job server is a loop: g9l_hello(), then g9l_fetch() and run the job, send the
 * result with g9l_send_result(), and g9l_wait_work() when the nRF5340 has no job.
 */

#ifndef SENSEI_GAP9_SIDE_H_
#define SENSEI_GAP9_SIDE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_link/pb.h"

/* Log level of the link: 0 errors only, 1 also warnings (default). Define before the first include. */
#ifndef G9L_LOG_LEVEL
#define G9L_LOG_LEVEL 1
#endif

#define G9L_FETCH_JOB 0  /* g9l_fetch(): a job was received */
#define G9L_FETCH_NONE 1 /* g9l_fetch(): the nRF5340 has no job */

struct g9l_cfg {
  uint32_t pad_sck, pad_mosi, pad_miso; /* SPI pads (pi_pad_e) */
  uint32_t pad_cs;                      /* chip select, a GPIO */
  uint32_t pad_trigger;                 /* trigger line from the nRF5340 (input) */
  uint32_t spi_hz;                      /* 0: G9L_SPI_HZ_DEFAULT */
  uint8_t spi_itf;                      /* SPI interface number */
};

/** Called by g9l_fetch() for a received job frame: decode into `job`. False if it is malformed (retry). */
typedef bool (*g9l_decode_fn)(const uint8_t *payload, size_t len, void *job);

/** Pads, SPI master, trigger line. */
int g9l_init(const struct g9l_cfg *cfg);

/** Send the hello frame (firmware version). Negative on failure after the retries. */
int g9l_hello(const sensei_Version *version);

/** Fetch the next job. G9L_FETCH_JOB, G9L_FETCH_NONE or a negative error (timeout, bad frames). */
int g9l_fetch(g9l_decode_fn decode, void *job);

/** Sleep until the nRF5340 raises the trigger line (after G9L_FETCH_NONE). */
void g9l_wait_work(void);

/** The camera is done with the shared I2C bus (job `seq`). */
int g9l_busfree(uint32_t seq);

/** Send an encoded result. more: another result frame of the same job follows (for example JPEG chunks). */
int g9l_send_result(const uint8_t *payload, size_t len, bool more);

#endif /* SENSEI_GAP9_SIDE_H_ */
