/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "ubx.h"

#include <errno.h>
#include <string.h>

enum {
  ST_SYNC1,
  ST_SYNC2,
  ST_CLASS,
  ST_ID,
  ST_LEN1,
  ST_LEN2,
  ST_PAYLOAD,
  ST_SKIP,
  ST_CK_A,
  ST_CK_B,
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
  wr16(p, (uint16_t)v);
  wr16(p + 2, (uint16_t)(v >> 16));
}

void ubx_checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b) {
  uint8_t a = 0;
  uint8_t b = 0;

  for (size_t i = 0; i < len; i++) {
    a = (uint8_t)(a + data[i]);
    b = (uint8_t)(b + a);
  }
  *ck_a = a;
  *ck_b = b;
}

int ubx_frame_encode(uint8_t *out, size_t out_size, uint8_t cls, uint8_t id, const uint8_t *payload, size_t len) {
  if (out == NULL || len > UINT16_MAX || (len > 0 && payload == NULL)) {
    return -EINVAL;
  }
  if (out_size < len + UBX_FRAME_OVERHEAD) {
    return -ENOSPC;
  }
  out[0] = UBX_SYNC1;
  out[1] = UBX_SYNC2;
  out[2] = cls;
  out[3] = id;
  wr16(&out[4], (uint16_t)len);
  if (len > 0) {
    memcpy(&out[UBX_HEADER_LEN], payload, len);
  }
  ubx_checksum(&out[2], len + 4, &out[UBX_HEADER_LEN + len], &out[UBX_HEADER_LEN + len + 1]);
  return (int)(len + UBX_FRAME_OVERHEAD);
}

int ubx_valset_init(struct ubx_valset *v, uint8_t *buf, size_t size, uint8_t layers) {
  if (size < UBX_HEADER_LEN + 4 + UBX_CHECKSUM_LEN) {
    return -ENOSPC;
  }
  v->buf = buf;
  v->size = size;
  buf[0] = UBX_SYNC1;
  buf[1] = UBX_SYNC2;
  buf[2] = UBX_CLASS_CFG;
  buf[3] = UBX_ID_CFG_VALSET;
  buf[UBX_HEADER_LEN + 0] = 0; /* version */
  buf[UBX_HEADER_LEN + 1] = layers;
  buf[UBX_HEADER_LEN + 2] = 0;
  buf[UBX_HEADER_LEN + 3] = 0;
  v->pos = UBX_HEADER_LEN + 4;
  return 0;
}

int ubx_valset_add(struct ubx_valset *v, uint32_t key, uint64_t value) {
  static const uint8_t sizes[] = {0, 1, 1, 2, 4, 8}; /* index: key bits 30..28, 1 = single bit stored as byte */
  unsigned int code = (key >> 28) & 0x7U;

  if (code == 0 || code >= (sizeof(sizes) / sizeof(sizes[0]))) {
    return -EINVAL;
  }
  size_t n = sizes[code];

  if (v->pos + 4 + n + UBX_CHECKSUM_LEN > v->size) {
    return -ENOSPC;
  }
  wr32(&v->buf[v->pos], key);
  for (size_t i = 0; i < n; i++) {
    v->buf[v->pos + 4 + i] = (uint8_t)(value >> (8 * i));
  }
  v->pos += 4 + n;
  return 0;
}

int ubx_valset_finish(struct ubx_valset *v) {
  size_t len = v->pos - UBX_HEADER_LEN;

  wr16(&v->buf[4], (uint16_t)len);
  ubx_checksum(&v->buf[2], len + 4, &v->buf[v->pos], &v->buf[v->pos + 1]);
  return (int)(v->pos + UBX_CHECKSUM_LEN);
}

int ubx_pmreq_encode(uint8_t *out, size_t out_size, uint32_t duration_ms, uint32_t flags, uint32_t wakeup) {
  uint8_t payload[16] = {0}; /* version 0, three reserved bytes */

  wr32(&payload[4], duration_ms);
  wr32(&payload[8], flags);
  wr32(&payload[12], wakeup);
  return ubx_frame_encode(out, out_size, UBX_CLASS_RXM, UBX_ID_RXM_PMREQ, payload, sizeof(payload));
}

int ubx_poll_encode(uint8_t *out, size_t out_size, uint8_t cls, uint8_t id) {
  return ubx_frame_encode(out, out_size, cls, id, NULL, 0);
}

void ubx_parser_init(struct ubx_parser *p, uint8_t *buf, size_t size) {
  memset(p, 0, sizeof(*p));
  p->buf = buf;
  p->size = (uint16_t)(size > UINT16_MAX ? UINT16_MAX : size);
  p->state = ST_SYNC1;
}

static void ck_add(struct ubx_parser *p, uint8_t byte) {
  p->ck_a = (uint8_t)(p->ck_a + byte);
  p->ck_b = (uint8_t)(p->ck_b + p->ck_a);
}

int ubx_parser_feed(struct ubx_parser *p, uint8_t byte, struct ubx_msg *msg) {
  switch (p->state) {
  case ST_SYNC1:
    if (byte == UBX_SYNC1) {
      p->state = ST_SYNC2;
    }
    break;
  case ST_SYNC2:
    if (byte == UBX_SYNC2) {
      p->state = ST_CLASS;
      p->ck_a = 0;
      p->ck_b = 0;
    } else {
      p->state = (byte == UBX_SYNC1) ? ST_SYNC2 : ST_SYNC1;
    }
    break;
  case ST_CLASS:
    p->cls = byte;
    ck_add(p, byte);
    p->state = ST_ID;
    break;
  case ST_ID:
    p->id = byte;
    ck_add(p, byte);
    p->state = ST_LEN1;
    break;
  case ST_LEN1:
    p->len = byte;
    ck_add(p, byte);
    p->state = ST_LEN2;
    break;
  case ST_LEN2:
    p->len |= (uint16_t)(byte << 8);
    ck_add(p, byte);
    p->pos = 0;
    if (p->len == 0) {
      p->state = ST_CK_A;
    } else {
      p->state = (p->len <= p->size) ? ST_PAYLOAD : ST_SKIP;
    }
    break;
  case ST_PAYLOAD:
    p->buf[p->pos++] = byte;
    ck_add(p, byte);
    if (p->pos == p->len) {
      p->state = ST_CK_A;
    }
    break;
  case ST_SKIP:
    /* Too long for the buffer: drop the payload, report after the checksum. */
    if (++p->pos == p->len) {
      p->state = ST_CK_A;
    }
    break;
  case ST_CK_A:
    p->rx_ck_a = byte;
    p->state = ST_CK_B;
    break;
  case ST_CK_B:
    p->state = ST_SYNC1;
    if (p->len > p->size) {
      return -EMSGSIZE;
    }
    if (p->rx_ck_a != p->ck_a || byte != p->ck_b) {
      return -EBADMSG;
    }
    msg->cls = p->cls;
    msg->id = p->id;
    msg->len = p->len;
    msg->payload = p->buf;
    return 1;
  default:
    p->state = ST_SYNC1;
    break;
  }
  return 0;
}

int ubx_parse_nav_pvt(const uint8_t *payload, size_t len, struct ubx_nav_pvt *out) {
  if (payload == NULL || out == NULL || len < UBX_NAV_PVT_LEN) {
    return -EINVAL;
  }
  out->itow_ms = rd32(&payload[0]);
  out->year = rd16(&payload[4]);
  out->month = payload[6];
  out->day = payload[7];
  out->hour = payload[8];
  out->min = payload[9];
  out->sec = payload[10];
  out->valid = payload[11];
  out->t_acc_ns = rd32(&payload[12]);
  out->nano = (int32_t)rd32(&payload[16]);
  out->fix_type = payload[20];
  out->flags = payload[21];
  out->flags2 = payload[22];
  out->num_sv = payload[23];
  out->lon_e7 = (int32_t)rd32(&payload[24]);
  out->lat_e7 = (int32_t)rd32(&payload[28]);
  out->height_mm = (int32_t)rd32(&payload[32]);
  out->hmsl_mm = (int32_t)rd32(&payload[36]);
  out->h_acc_mm = rd32(&payload[40]);
  out->v_acc_mm = rd32(&payload[44]);
  out->vel_n_mmps = (int32_t)rd32(&payload[48]);
  out->vel_e_mmps = (int32_t)rd32(&payload[52]);
  out->vel_d_mmps = (int32_t)rd32(&payload[56]);
  out->gspeed_mmps = (int32_t)rd32(&payload[60]);
  out->head_mot_e5 = (int32_t)rd32(&payload[64]);
  out->s_acc_mmps = rd32(&payload[68]);
  out->head_acc_e5 = rd32(&payload[72]);
  out->pdop_e2 = rd16(&payload[76]);
  out->flags3 = rd16(&payload[78]);
  return 0;
}

int ubx_nav_sat_count(const uint8_t *payload, size_t len) {
  if (payload == NULL || len < UBX_NAV_SAT_HEADER_LEN) {
    return -EINVAL;
  }
  size_t n = payload[5];

  if (len < UBX_NAV_SAT_HEADER_LEN + n * UBX_NAV_SAT_ENTRY_LEN) {
    return -EINVAL;
  }
  return (int)n;
}

int ubx_nav_sat_get(const uint8_t *payload, size_t len, unsigned int idx, struct ubx_sat *out) {
  int n = ubx_nav_sat_count(payload, len);

  if (n < 0 || idx >= (unsigned int)n || out == NULL) {
    return -EINVAL;
  }
  const uint8_t *e = &payload[UBX_NAV_SAT_HEADER_LEN + idx * UBX_NAV_SAT_ENTRY_LEN];

  out->gnss_id = e[0];
  out->sv_id = e[1];
  out->cno_dbhz = e[2];
  out->elev_deg = (int8_t)e[3];
  out->azim_deg = (int16_t)rd16(&e[4]);
  out->flags = rd32(&e[8]);
  return 0;
}

int ubx_ack_result(const struct ubx_msg *msg, uint8_t cls, uint8_t id) {
  if (msg->cls != UBX_CLASS_ACK || msg->len != 2 || msg->payload[0] != cls || msg->payload[1] != id) {
    return -EINVAL;
  }
  if (msg->id == UBX_ID_ACK_ACK) {
    return 1;
  }
  return msg->id == UBX_ID_ACK_NAK ? 0 : -EINVAL;
}
