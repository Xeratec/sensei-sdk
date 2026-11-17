/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>

#include "ubx.h"

static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, v & 0xFFFF);
  put16(p + 2, v >> 16);
}

/* A NAV-PVT payload with known values. */
static void make_pvt(uint8_t *pl) {
  memset(pl, 0, UBX_NAV_PVT_LEN);
  put32(&pl[0], 123456);             /* iTOW */
  put16(&pl[4], 2026);               /* year */
  pl[6] = 11;                        /* month */
  pl[7] = 17;                        /* day */
  pl[8] = 13;                        /* hour */
  pl[9] = 45;                        /* min */
  pl[10] = 59;                       /* sec */
  pl[11] = 0x03;                     /* validDate, validTime */
  put32(&pl[12], 25);                /* tAcc */
  put32(&pl[16], (uint32_t)-500000); /* nano */
  pl[20] = UBX_FIX_3D;
  pl[21] = UBX_PVT_FLAGS_GNSS_FIX_OK;
  pl[23] = 9;                          /* numSV */
  put32(&pl[24], (uint32_t)85432100);  /* lon 8.54321 */
  put32(&pl[28], (uint32_t)474100000); /* lat 47.41 */
  put32(&pl[32], 450000);              /* height */
  put32(&pl[36], 408000);              /* hMSL */
  put32(&pl[40], 3200);                /* hAcc */
  put32(&pl[60], 1500);                /* gSpeed */
  put32(&pl[64], (uint32_t)-9000000);  /* headMot -90 deg */
  put16(&pl[76], 123);                 /* pDOP */
}

ZTEST(ubx, test_checksum_known_frames) {
  uint8_t a, b;
  /* UBX-MON-VER poll: B5 62 0A 04 00 00 0E 34 */
  const uint8_t mon_ver[] = {0x0A, 0x04, 0x00, 0x00};
  ubx_checksum(mon_ver, sizeof(mon_ver), &a, &b);
  zassert_equal(a, 0x0E);
  zassert_equal(b, 0x34);
  /* UBX-ACK-ACK for CFG-VALSET: B5 62 05 01 02 00 06 8A 98 C1 */
  const uint8_t ack[] = {0x05, 0x01, 0x02, 0x00, 0x06, 0x8A};
  ubx_checksum(ack, sizeof(ack), &a, &b);
  zassert_equal(a, 0x98);
  zassert_equal(b, 0xC1);
}

ZTEST(ubx, test_poll_frame) {
  uint8_t out[16];
  int n = ubx_poll_encode(out, sizeof(out), UBX_CLASS_NAV, UBX_ID_NAV_PVT);
  const uint8_t expect[] = {0xB5, 0x62, 0x01, 0x07, 0x00, 0x00, 0x08, 0x19};
  zassert_equal(n, sizeof(expect));
  zassert_mem_equal(out, expect, sizeof(expect));
  zassert_equal(ubx_poll_encode(out, 7, UBX_CLASS_NAV, UBX_ID_NAV_PVT), -ENOSPC);
}

ZTEST(ubx, test_valset_builder) {
  uint8_t buf[64];
  struct ubx_valset v;
  zassert_ok(ubx_valset_init(&v, buf, sizeof(buf), UBX_LAYER_RAM | UBX_LAYER_BBR));
  zassert_ok(ubx_valset_add(&v, UBX_KEY_I2COUTPROT_NMEA, 0));       /* L: 1 byte */
  zassert_ok(ubx_valset_add(&v, UBX_KEY_MSGOUT_NAV_PVT_I2C, 1));    /* U1 */
  zassert_ok(ubx_valset_add(&v, UBX_KEY_RATE_MEAS, 1000));          /* U2 */
  zassert_ok(ubx_valset_add(&v, UBX_KEY_PM_POSUPDATEPERIOD, 3600)); /* U4 */
  int n = ubx_valset_finish(&v);
  /* 6 header + 4 + (4+1) + (4+1) + (4+2) + (4+4) + 2 checksum */
  zassert_equal(n, 6 + 4 + 5 + 5 + 6 + 8 + 2);
  zassert_equal(buf[2], UBX_CLASS_CFG);
  zassert_equal(buf[3], UBX_ID_CFG_VALSET);
  zassert_equal(buf[4] | (buf[5] << 8), n - UBX_FRAME_OVERHEAD);
  zassert_equal(buf[7], UBX_LAYER_RAM | UBX_LAYER_BBR);
  /* First key little endian, then the value. */
  const uint8_t key0[] = {0x02, 0x00, 0x72, 0x10, 0x00};
  zassert_mem_equal(&buf[10], key0, sizeof(key0));
  /* The U2 value 1000 = 0x03E8. */
  zassert_equal(buf[10 + 5 + 5 + 4], 0xE8);
  zassert_equal(buf[10 + 5 + 5 + 5], 0x03);

  /* The frame decodes with the parser, so the checksum is right. */
  struct ubx_parser p;
  uint8_t rx[64];
  struct ubx_msg m;
  ubx_parser_init(&p, rx, sizeof(rx));
  int r = 0;
  for (int i = 0; i < n; i++) {
    r = ubx_parser_feed(&p, buf[i], &m);
  }
  zassert_equal(r, 1);
  zassert_equal(m.cls, UBX_CLASS_CFG);
  zassert_equal(m.len, n - UBX_FRAME_OVERHEAD);

  /* A key of unknown size and a full buffer are rejected. */
  zassert_equal(ubx_valset_add(&v, 0x00000001UL, 0), -EINVAL);
  struct ubx_valset small;
  uint8_t sb[16];
  zassert_ok(ubx_valset_init(&small, sb, sizeof(sb), UBX_LAYER_RAM));
  zassert_equal(ubx_valset_add(&small, UBX_KEY_PM_POSUPDATEPERIOD, 5), -ENOSPC);
}

ZTEST(ubx, test_pmreq) {
  uint8_t out[32];
  int n = ubx_pmreq_encode(out, sizeof(out), 0, UBX_PMREQ_FLAG_BACKUP, UBX_PMREQ_WAKE_EXTINT0);
  zassert_equal(n, 16 + UBX_FRAME_OVERHEAD);
  zassert_equal(out[2], UBX_CLASS_RXM);
  zassert_equal(out[3], UBX_ID_RXM_PMREQ);
  zassert_equal(out[4], 16);
  zassert_equal(out[UBX_HEADER_LEN + 8], UBX_PMREQ_FLAG_BACKUP);
  zassert_equal(out[UBX_HEADER_LEN + 12], UBX_PMREQ_WAKE_EXTINT0);
  uint8_t a, b;
  ubx_checksum(&out[2], 4 + 16, &a, &b);
  zassert_equal(out[n - 2], a);
  zassert_equal(out[n - 1], b);
}

ZTEST(ubx, test_parse_nav_pvt) {
  uint8_t pl[UBX_NAV_PVT_LEN];
  struct ubx_nav_pvt pvt;
  make_pvt(pl);
  zassert_ok(ubx_parse_nav_pvt(pl, sizeof(pl), &pvt));
  zassert_equal(pvt.itow_ms, 123456);
  zassert_equal(pvt.year, 2026);
  zassert_equal(pvt.month, 11);
  zassert_equal(pvt.day, 17);
  zassert_equal(pvt.hour, 13);
  zassert_equal(pvt.min, 45);
  zassert_equal(pvt.sec, 59);
  zassert_equal(pvt.valid & 0x3, 0x3);
  zassert_equal(pvt.nano, -500000);
  zassert_equal(pvt.fix_type, UBX_FIX_3D);
  zassert_equal(pvt.num_sv, 9);
  zassert_equal(pvt.lon_e7, 85432100);
  zassert_equal(pvt.lat_e7, 474100000);
  zassert_equal(pvt.hmsl_mm, 408000);
  zassert_equal(pvt.h_acc_mm, 3200);
  zassert_equal(pvt.gspeed_mmps, 1500);
  zassert_equal(pvt.head_mot_e5, -9000000);
  zassert_equal(pvt.pdop_e2, 123);
  zassert_equal(ubx_parse_nav_pvt(pl, UBX_NAV_PVT_LEN - 1, &pvt), -EINVAL);
}

/* Garbage, a bad frame and a good frame, fed byte by byte. */
ZTEST(ubx, test_parser_stream) {
  uint8_t pl[UBX_NAV_PVT_LEN];
  uint8_t frame[UBX_NAV_PVT_LEN + UBX_FRAME_OVERHEAD];
  uint8_t bad[UBX_NAV_PVT_LEN + UBX_FRAME_OVERHEAD];
  uint8_t rx[128];
  struct ubx_parser p;
  struct ubx_msg m;

  make_pvt(pl);
  zassert_equal(ubx_frame_encode(frame, sizeof(frame), UBX_CLASS_NAV, UBX_ID_NAV_PVT, pl, sizeof(pl)), sizeof(frame));
  memcpy(bad, frame, sizeof(bad));
  bad[30] ^= 0x01;

  ubx_parser_init(&p, rx, sizeof(rx));
  int got = 0, bad_cnt = 0;
  /* Noise before the first sync, including a lone sync char and a repeated one. */
  const uint8_t junk[] = {0xFF, 0xFF, 0xB5, 0x00, 0xB5, 0xB5, 0x00, 0x62};
  for (size_t i = 0; i < sizeof(junk); i++) {
    zassert_equal(ubx_parser_feed(&p, junk[i], &m), 0);
  }
  for (size_t i = 0; i < sizeof(bad); i++) {
    int r = ubx_parser_feed(&p, bad[i], &m);
    if (r == -EBADMSG) {
      bad_cnt++;
    }
  }
  for (size_t i = 0; i < sizeof(frame); i++) {
    int r = ubx_parser_feed(&p, frame[i], &m);
    if (r == 1) {
      got++;
      struct ubx_nav_pvt pvt;
      zassert_equal(m.cls, UBX_CLASS_NAV);
      zassert_equal(m.id, UBX_ID_NAV_PVT);
      zassert_equal(m.len, UBX_NAV_PVT_LEN);
      zassert_ok(ubx_parse_nav_pvt(m.payload, m.len, &pvt));
      zassert_equal(pvt.lat_e7, 474100000);
    }
  }
  zassert_equal(bad_cnt, 1);
  zassert_equal(got, 1);
}

ZTEST(ubx, test_parser_oversized_and_empty) {
  uint8_t big_pl[40] = {0};
  uint8_t big[40 + UBX_FRAME_OVERHEAD];
  uint8_t poll[8];
  uint8_t rx[16];
  struct ubx_parser p;
  struct ubx_msg m;

  zassert_equal(ubx_frame_encode(big, sizeof(big), UBX_CLASS_NAV, UBX_ID_NAV_SAT, big_pl, sizeof(big_pl)), sizeof(big));
  zassert_equal(ubx_poll_encode(poll, sizeof(poll), UBX_CLASS_MON, UBX_ID_MON_VER), sizeof(poll));

  ubx_parser_init(&p, rx, sizeof(rx));
  int too_big = 0, ok = 0;
  for (size_t i = 0; i < sizeof(big); i++) {
    if (ubx_parser_feed(&p, big[i], &m) == -EMSGSIZE) {
      too_big++;
    }
  }
  zassert_equal(too_big, 1);
  /* The parser is back in sync: an empty frame follows. */
  for (size_t i = 0; i < sizeof(poll); i++) {
    if (ubx_parser_feed(&p, poll[i], &m) == 1) {
      ok++;
      zassert_equal(m.len, 0);
      zassert_equal(m.id, UBX_ID_MON_VER);
    }
  }
  zassert_equal(ok, 1);
}

ZTEST(ubx, test_nav_sat_and_ack) {
  uint8_t pl[UBX_NAV_SAT_HEADER_LEN + 2 * UBX_NAV_SAT_ENTRY_LEN] = {0};
  struct ubx_sat s;

  pl[5] = 2; /* numSvs */
  uint8_t *e = &pl[UBX_NAV_SAT_HEADER_LEN];
  e[0] = UBX_GNSS_GALILEO;
  e[1] = 12;
  e[2] = 41;
  e[3] = (uint8_t)-5; /* elev -5 deg */
  put16(&e[4], 270);
  put32(&e[8], 0x08 | 0x04); /* used, quality 4 */
  e += UBX_NAV_SAT_ENTRY_LEN;
  e[0] = UBX_GNSS_GPS;
  e[1] = 3;
  e[2] = 30;
  e[3] = 60;
  put16(&e[4], 15);

  zassert_equal(ubx_nav_sat_count(pl, sizeof(pl)), 2);
  zassert_equal(ubx_nav_sat_count(pl, sizeof(pl) - 1), -EINVAL);
  zassert_ok(ubx_nav_sat_get(pl, sizeof(pl), 0, &s));
  zassert_equal(s.gnss_id, UBX_GNSS_GALILEO);
  zassert_equal(s.sv_id, 12);
  zassert_equal(s.cno_dbhz, 41);
  zassert_equal(s.elev_deg, -5);
  zassert_equal(s.azim_deg, 270);
  zassert_equal(UBX_SAT_USED(s.flags), 1);
  zassert_equal(UBX_SAT_QUALITY(s.flags), 4);
  zassert_ok(ubx_nav_sat_get(pl, sizeof(pl), 1, &s));
  zassert_equal(s.gnss_id, UBX_GNSS_GPS);
  zassert_equal(UBX_SAT_USED(s.flags), 0);
  zassert_equal(ubx_nav_sat_get(pl, sizeof(pl), 2, &s), -EINVAL);

  /* ACK and NAK for CFG-VALSET. */
  static const uint8_t ack_pl[] = {UBX_CLASS_CFG, UBX_ID_CFG_VALSET};
  struct ubx_msg m = {.cls = UBX_CLASS_ACK, .id = UBX_ID_ACK_ACK, .len = 2, .payload = ack_pl};
  zassert_equal(ubx_ack_result(&m, UBX_CLASS_CFG, UBX_ID_CFG_VALSET), 1);
  m.id = UBX_ID_ACK_NAK;
  zassert_equal(ubx_ack_result(&m, UBX_CLASS_CFG, UBX_ID_CFG_VALSET), 0);
  zassert_equal(ubx_ack_result(&m, UBX_CLASS_CFG, UBX_ID_CFG_RST), -EINVAL);
}

ZTEST_SUITE(ubx, NULL, NULL, NULL, NULL, NULL);
