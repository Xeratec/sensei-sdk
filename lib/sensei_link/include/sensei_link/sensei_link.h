/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Constants of the SENSEI link layer: framing version, stream channels, SPI frame types and limits.
 */
#ifndef SENSEI_LINK_H
#define SENSEI_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version of the framing (stream, SPI, fragments). Bump on incompatible changes.
 * 2: the SPI frame CRC covers header bytes 0 to 5 and the payload (was: payload only). */
#define SENSEI_LINK_VERSION 2

/* Largest payload of an SPI frame, the same on both ends of a link. Set it for the whole build, never
 * per file: Zephyr CONFIG_SENSEI_SPI_MAX_PAYLOAD, CMake -DSENSEI_SPI_MAX_PAYLOAD=<n> (sensei_link.cmake
 * adds it to SENSEI_LINK_DEFINITIONS). Default 8192. */
#ifndef SENSEI_SPI_MAX_PAYLOAD
#ifdef CONFIG_SENSEI_SPI_MAX_PAYLOAD
#define SENSEI_SPI_MAX_PAYLOAD CONFIG_SENSEI_SPI_MAX_PAYLOAD
#else
#define SENSEI_SPI_MAX_PAYLOAD 8192u
#endif
#endif

/* Largest message on a byte stream (USB, UART, BLE NUS), the same on both ends of a link. Receivers size their
 * buffer with SENSEI_STREAM_FRAME_MAX(SENSEI_STREAM_MAX_MSG) (stream.h). An app whose messages are larger has to
 * raise it for every peer. */
#define SENSEI_STREAM_MAX_MSG 2144u

/* Radio payload limits, chunk header included (frag.h). */
#define SENSEI_MTU_LORA 222u
#define SENSEI_MTU_BLE 244u
#define SENSEI_MTU_LTE 1024u

/* Channel byte of a stream frame (stream.h). The values are on the wire: never change or reuse one.
 *   0x00         invalid
 *   0x01 - 0x3F  SDK (core range). 0x08 - 0x3F is reserved for later SDK channels (link control, log, ...).
 *   0x40 - 0xFF  application: defined by the app, same value on both ends, never used by the SDK.
 * The SDK does not fix the schema of the messages on 0x01 to 0x06: the app defines them. */
#define SENSEI_CH_SDK_FIRST 0x01u
#define SENSEI_CH_SDK_LAST 0x3Fu
#define SENSEI_CH_APP_FIRST 0x40u
#define SENSEI_CH_APP_LAST 0xFFu

typedef enum {
  SENSEI_CH_HOST_TO_NODE = 0x01,   /* protobuf stream, host to device (the app's request message) */
  SENSEI_CH_NODE_TO_HOST = 0x02,   /* protobuf stream, device to host (the app's reply and event message) */
  SENSEI_CH_UPLINK = 0x03,         /* uplink message, whole */
  SENSEI_CH_DOWNLINK = 0x04,       /* downlink message, whole */
  SENSEI_CH_UPLINK_CHUNK = 0x05,   /* one fragment of an uplink (frag.h) */
  SENSEI_CH_DOWNLINK_CHUNK = 0x06, /* one fragment of a downlink (frag.h) */
  SENSEI_CH_SEALED = 0x07,         /* sealed envelope of sensei_secure, its kind is in the header */
} sensei_channel_t;

/* Type byte of an SPI frame. 0x10 to 0x1F: GAP9 link (see sensei_gap9), 0x20 to 0x2F: LPWAN shield. */
typedef enum {
  SENSEI_SPI_GAP9_JOB = 0x10,       /* job, nRF5340 to GAP9 */
  SENSEI_SPI_GAP9_RESULT = 0x11,    /* result, GAP9 to nRF5340 */
  SENSEI_SPI_LPWAN_REQUEST = 0x20,  /* LpwanRequest, nRF5340 to nRF9151 */
  SENSEI_SPI_LPWAN_RESPONSE = 0x21, /* LpwanResponse */
  SENSEI_SPI_LPWAN_EVENT = 0x22,    /* LpwanEvent */
} sensei_spi_type_t;

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LINK_H */
