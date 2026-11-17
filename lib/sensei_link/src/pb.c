/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_link/pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

int32_t sensei_pb_encode(uint8_t *buf, size_t cap, const pb_msgdesc_t *fields, const void *msg) {
  pb_ostream_t os = pb_ostream_from_buffer(buf, cap);

  if (!pb_encode(&os, fields, msg)) {
    return -1;
  }
  return (int32_t)os.bytes_written;
}

bool sensei_pb_decode(const uint8_t *buf, size_t len, const pb_msgdesc_t *fields, void *msg) {
  pb_istream_t is = pb_istream_from_buffer(buf, len);

  return pb_decode(&is, fields, msg);
}
