/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Coredump backend "other": the Zephyr coredump goes into the retention area sensei,diag-dump. Layout: magic u32,
 * data length u32, then the dump. What does not fit is dropped (the registers come first, then the stack top). */

#include <errno.h>
#include <string.h>

#include <zephyr/debug/coredump.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/retention/retention.h>
#include <zephyr/sys/byteorder.h>

#include "diag/sensei_diag_rt.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_DEBUG_COREDUMP_BACKEND_OTHER),
             "SENSEI_DIAG_COREDUMP needs DEBUG_COREDUMP_BACKEND_OTHER=y");

#define DUMP_MAGIC 0x504D4453u /* "SDMP" */
#define HDR_BYTES 8u
#define DUMP_AREA DT_REG_SIZE(DT_CHOSEN(sensei_diag_dump))

static const struct device *const dump_dev = DEVICE_DT_GET(DT_CHOSEN(sensei_diag_dump));
static size_t written; /* data bytes of the dump in progress */

static void header_write(uint32_t magic, uint32_t len) {
  uint8_t h[HDR_BYTES];

  sys_put_le32(magic, h);
  sys_put_le32(len, h + 4);
  (void)retention_write(dump_dev, 0, h, sizeof(h));
}

static bool header_read(uint32_t *len) {
  uint8_t h[HDR_BYTES];

  if (retention_read(dump_dev, 0, h, sizeof(h)) != 0 || sys_get_le32(h) != DUMP_MAGIC) {
    return false;
  }
  *len = sys_get_le32(h + 4);
  return *len <= DUMP_AREA - HDR_BYTES;
}

bool sensei_diag_dump_valid(void) {
  uint32_t len;

  return header_read(&len) && len > 0;
}

size_t sensei_diag_dump_size(void) {
  uint32_t len;

  return header_read(&len) ? len : 0;
}

size_t sensei_diag_dump_read(size_t offset, uint8_t *buf, size_t max) {
  size_t len = sensei_diag_dump_size();

  if (offset >= len) {
    return 0;
  }
  if (max > len - offset) {
    max = len - offset;
  }
  return retention_read(dump_dev, HDR_BYTES + offset, buf, max) == 0 ? max : 0;
}

void sensei_diag_dump_clear(void) { header_write(0, 0); }

static void backend_start(void) {
  written = 0;
  header_write(0, 0);
}

static void backend_end(void) { header_write(DUMP_MAGIC, (uint32_t)written); }

static void backend_buffer_output(uint8_t *buf, size_t buflen) {
  size_t room = DUMP_AREA - HDR_BYTES - written;

  if (buflen > room) {
    buflen = room;
  }
  if (buflen > 0 && retention_write(dump_dev, HDR_BYTES + written, buf, buflen) == 0) {
    written += buflen;
  }
}

static int backend_query(enum coredump_query_id id, void *arg) {
  ARG_UNUSED(arg);
  switch (id) {
  case COREDUMP_QUERY_GET_ERROR:
    return 0;
  case COREDUMP_QUERY_HAS_STORED_DUMP:
    return sensei_diag_dump_valid() ? 1 : 0;
  case COREDUMP_QUERY_GET_STORED_DUMP_SIZE:
    return (int)sensei_diag_dump_size();
  default:
    return -ENOTSUP;
  }
}

static int backend_cmd(enum coredump_cmd_id id, void *arg) {
  switch (id) {
  case COREDUMP_CMD_CLEAR_ERROR:
    return 0;
  case COREDUMP_CMD_VERIFY_STORED_DUMP:
    return sensei_diag_dump_valid() ? 1 : 0;
  case COREDUMP_CMD_ERASE_STORED_DUMP:
  case COREDUMP_CMD_INVALIDATE_STORED_DUMP:
    sensei_diag_dump_clear();
    return 0;
  case COREDUMP_CMD_COPY_STORED_DUMP: {
    struct coredump_cmd_copy_arg *c = arg;

    if (c == NULL || c->buffer == NULL || c->offset < 0) {
      return -EINVAL;
    }
    return sensei_diag_dump_read((size_t)c->offset, c->buffer, c->length) == c->length ? 0 : -EIO;
  }
  default:
    return -ENOTSUP;
  }
}

struct coredump_backend_api coredump_backend_other = {
    .start = backend_start,
    .end = backend_end,
    .buffer_output = backend_buffer_output,
    .query = backend_query,
    .cmd = backend_cmd,
};
