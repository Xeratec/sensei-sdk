/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 *
 * Thin wrappers around nanopb. Messages live in caller-provided structs; nothing is allocated.
 */
#ifndef SENSEI_LINK_PB_H
#define SENSEI_LINK_PB_H

#include <pb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei/lpwan.pb.h"
#include "sensei/types.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encode `msg` into `buf`. Returns the encoded length, or -1 if it does not fit or is invalid. */
int32_t sensei_pb_encode(uint8_t *buf, size_t cap, const pb_msgdesc_t *fields, const void *msg);

/* Decode into `msg` (set to defaults first). Returns false on malformed input. */
bool sensei_pb_decode(const uint8_t *buf, size_t len, const pb_msgdesc_t *fields, void *msg);

/* Typed helpers: sensei_encode_<Name>(buf, cap, &msg) and sensei_decode_<Name>(buf, len, &msg). */
#define SENSEI_PB_DECLARE(Name)                                                                                        \
  static inline int32_t sensei_encode_##Name(uint8_t *buf, size_t cap, const sensei_##Name *m) {                       \
    return sensei_pb_encode(buf, cap, &sensei_##Name##_msg, m);                                                        \
  }                                                                                                                    \
  static inline bool sensei_decode_##Name(const uint8_t *buf, size_t len, sensei_##Name *m) {                          \
    return sensei_pb_decode(buf, len, &sensei_##Name##_msg, m);                                                        \
  }

SENSEI_PB_DECLARE(Version)
SENSEI_PB_DECLARE(LpwanRequest)
SENSEI_PB_DECLARE(LpwanResponse)
SENSEI_PB_DECLARE(LpwanEvent)

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_LINK_PB_H */
