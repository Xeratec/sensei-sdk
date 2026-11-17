/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file ubx.h
 * @brief Minimal u-blox UBX protocol helpers: framing, checksum, CFG-VALSET builder and parsers for
 *        UBX-NAV-PVT and UBX-NAV-SAT. No Zephyr dependency, so it also builds on the host.
 */

#ifndef SENSEI_UBX_H_
#define SENSEI_UBX_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UBX_SYNC1 0xB5U
#define UBX_SYNC2 0x62U
/** Sync, class, id, length (2 bytes). */
#define UBX_HEADER_LEN 6U
/** CK_A and CK_B. */
#define UBX_CHECKSUM_LEN 2U
#define UBX_FRAME_OVERHEAD (UBX_HEADER_LEN + UBX_CHECKSUM_LEN)

/* Message classes and ids used by the driver. */
#define UBX_CLASS_NAV 0x01U
#define UBX_CLASS_RXM 0x02U
#define UBX_CLASS_ACK 0x05U
#define UBX_CLASS_CFG 0x06U
#define UBX_CLASS_MON 0x0AU

#define UBX_ID_NAV_PVT 0x07U
#define UBX_ID_NAV_SAT 0x35U
#define UBX_ID_ACK_NAK 0x00U
#define UBX_ID_ACK_ACK 0x01U
#define UBX_ID_CFG_RST 0x04U
#define UBX_ID_CFG_VALSET 0x8AU
#define UBX_ID_MON_VER 0x04U
#define UBX_ID_RXM_PMREQ 0x41U

/** UBX-NAV-PVT payload length. */
#define UBX_NAV_PVT_LEN 92U
/** UBX-NAV-SAT payload: 8 byte header and 12 bytes per satellite. */
#define UBX_NAV_SAT_HEADER_LEN 8U
#define UBX_NAV_SAT_ENTRY_LEN 12U

/* Configuration keys (u-blox M10 SPG 5.10 interface description). The size is in bits 30..28. */
#define UBX_KEY_I2CINPROT_UBX 0x10710001UL
#define UBX_KEY_I2CINPROT_NMEA 0x10710002UL
#define UBX_KEY_I2COUTPROT_UBX 0x10720001UL
#define UBX_KEY_I2COUTPROT_NMEA 0x10720002UL
#define UBX_KEY_I2C_EXTENDEDTIMEOUT 0x10510002UL
#define UBX_KEY_MSGOUT_NAV_PVT_I2C 0x20910006UL
#define UBX_KEY_MSGOUT_NAV_SAT_I2C 0x20910015UL
#define UBX_KEY_RATE_MEAS 0x30210001UL
#define UBX_KEY_RATE_NAV 0x30210002UL
#define UBX_KEY_NAVSPG_DYNMODEL 0x20110021UL
#define UBX_KEY_PM_OPERATEMODE 0x20D00001UL
#define UBX_KEY_PM_POSUPDATEPERIOD 0x40D00002UL
#define UBX_KEY_PM_ACQPERIOD 0x40D00003UL
#define UBX_KEY_PM_ONTIME 0x30D00005UL
#define UBX_KEY_PM_EXTINTWAKE 0x10D0000CUL
#define UBX_KEY_PM_EXTINTBACKUP 0x10D0000DUL
#define UBX_KEY_SIGNAL_GPS_ENA 0x1031001FUL
#define UBX_KEY_SIGNAL_SBAS_ENA 0x10310020UL
#define UBX_KEY_SIGNAL_GAL_ENA 0x10310021UL
#define UBX_KEY_SIGNAL_BDS_ENA 0x10310022UL
#define UBX_KEY_SIGNAL_QZSS_ENA 0x10310024UL
#define UBX_KEY_SIGNAL_GLO_ENA 0x10310025UL

/** CFG-VALSET layers. */
#define UBX_LAYER_RAM 0x01U
#define UBX_LAYER_BBR 0x02U
#define UBX_LAYER_FLASH 0x04U

/** CFG-PM-OPERATEMODE values. */
#define UBX_PM_FULL 0U
#define UBX_PM_PSMOO 1U
#define UBX_PM_PSMCT 2U

/** CFG-NAVSPG-DYNMODEL values. */
#define UBX_DYN_PORTABLE 0U
#define UBX_DYN_STATIONARY 2U
#define UBX_DYN_PEDESTRIAN 3U
#define UBX_DYN_AUTOMOTIVE 4U
#define UBX_DYN_AIRBORNE_4G 8U

/** UBX-RXM-PMREQ flags and wake-up sources. */
#define UBX_PMREQ_FLAG_BACKUP 0x02U
#define UBX_PMREQ_FLAG_FORCE 0x04U
#define UBX_PMREQ_WAKE_UARTRX 0x08U
#define UBX_PMREQ_WAKE_EXTINT0 0x20U
#define UBX_PMREQ_WAKE_EXTINT1 0x40U

/** UBX-NAV-PVT valid and flag bits. */
#define UBX_PVT_VALID_DATE 0x01U
#define UBX_PVT_VALID_TIME 0x02U
#define UBX_PVT_FLAGS_GNSS_FIX_OK 0x01U
#define UBX_PVT_FLAGS_DIFF_SOLN 0x02U
#define UBX_PVT_FLAGS3_INVALID_LLH 0x0001U

/** UBX-NAV-PVT fixType. */
enum ubx_fix_type {
  UBX_FIX_NONE = 0,
  UBX_FIX_DEAD_RECKONING = 1,
  UBX_FIX_2D = 2,
  UBX_FIX_3D = 3,
  UBX_FIX_GNSS_DR = 4,
  UBX_FIX_TIME_ONLY = 5,
};

/** UBX-NAV-SAT gnssId. */
enum ubx_gnss_id {
  UBX_GNSS_GPS = 0,
  UBX_GNSS_SBAS = 1,
  UBX_GNSS_GALILEO = 2,
  UBX_GNSS_BEIDOU = 3,
  UBX_GNSS_QZSS = 5,
  UBX_GNSS_GLONASS = 6,
};

/** Decoded UBX-NAV-PVT. Units as in the interface description unless noted. */
struct ubx_nav_pvt {
  uint32_t itow_ms;
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t min;
  uint8_t sec;
  uint8_t valid;
  uint32_t t_acc_ns;
  int32_t nano;
  uint8_t fix_type;
  uint8_t flags;
  uint8_t flags2;
  uint8_t num_sv;
  int32_t lon_e7;    /**< degrees * 1e-7 */
  int32_t lat_e7;    /**< degrees * 1e-7 */
  int32_t height_mm; /**< above ellipsoid */
  int32_t hmsl_mm;   /**< above mean sea level */
  uint32_t h_acc_mm;
  uint32_t v_acc_mm;
  int32_t vel_n_mmps;
  int32_t vel_e_mmps;
  int32_t vel_d_mmps;
  int32_t gspeed_mmps;
  int32_t head_mot_e5; /**< degrees * 1e-5 */
  uint32_t s_acc_mmps;
  uint32_t head_acc_e5;
  uint16_t pdop_e2; /**< 0.01 */
  uint16_t flags3;
};

/** One satellite of UBX-NAV-SAT. */
struct ubx_sat {
  uint8_t gnss_id;
  uint8_t sv_id;
  uint8_t cno_dbhz;
  int8_t elev_deg;
  int16_t azim_deg;
  uint32_t flags;
};

#define UBX_SAT_QUALITY(flags) ((flags) & 0x7U)
#define UBX_SAT_USED(flags) (((flags) >> 3) & 0x1U)
#define UBX_SAT_DIFF_CORR(flags) (((flags) >> 6) & 0x1U)

/** A received message. The payload points into the parser buffer. */
struct ubx_msg {
  uint8_t cls;
  uint8_t id;
  uint16_t len;
  const uint8_t *payload;
};

/** Byte-wise frame parser. Resynchronises by itself after garbage or a bad checksum. */
struct ubx_parser {
  uint8_t *buf;
  uint16_t size;
  uint16_t pos;
  uint16_t len;
  uint8_t state;
  uint8_t cls;
  uint8_t id;
  uint8_t ck_a;
  uint8_t ck_b;
  uint8_t rx_ck_a;
};

/**
 * @brief Fletcher checksum over class, id, length and payload (the bytes after the sync chars).
 */
void ubx_checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b);

/**
 * @brief Build a complete frame.
 * @return the frame length, or a negative errno (-EINVAL, -ENOSPC)
 */
int ubx_frame_encode(uint8_t *out, size_t out_size, uint8_t cls, uint8_t id, const uint8_t *payload, size_t len);

/** CFG-VALSET builder. Fill with ubx_valset_add(), then ubx_valset_finish(). */
struct ubx_valset {
  uint8_t *buf;
  size_t size;
  size_t pos;
};

/** Start a CFG-VALSET frame in buf. @return 0 or -ENOSPC. */
int ubx_valset_init(struct ubx_valset *v, uint8_t *buf, size_t size, uint8_t layers);

/**
 * @brief Append a key and its value. The value size comes from the key.
 * @return 0, -ENOSPC or -EINVAL (unknown size, or more than 64 pairs is the caller's duty to avoid)
 */
int ubx_valset_add(struct ubx_valset *v, uint32_t key, uint64_t value);

/** Fill in length and checksum. @return the frame length. */
int ubx_valset_finish(struct ubx_valset *v);

/** Build a UBX-RXM-PMREQ frame. @return the frame length or a negative errno. */
int ubx_pmreq_encode(uint8_t *out, size_t out_size, uint32_t duration_ms, uint32_t flags, uint32_t wakeup);

/** Build an empty poll request (for example UBX-NAV-PVT). @return the frame length or a negative errno. */
int ubx_poll_encode(uint8_t *out, size_t out_size, uint8_t cls, uint8_t id);

void ubx_parser_init(struct ubx_parser *p, uint8_t *buf, size_t size);

/**
 * @brief Feed one byte.
 * @return 1 if a message is complete (msg is filled and valid until the next call),
 *         0 if more bytes are needed,
 *         -EBADMSG on a checksum error, -EMSGSIZE if the payload does not fit the buffer (it is skipped).
 */
int ubx_parser_feed(struct ubx_parser *p, uint8_t byte, struct ubx_msg *msg);

/** Decode a UBX-NAV-PVT payload. @return 0 or -EINVAL. */
int ubx_parse_nav_pvt(const uint8_t *payload, size_t len, struct ubx_nav_pvt *out);

/** Number of satellites in a UBX-NAV-SAT payload, or -EINVAL if the length does not match. */
int ubx_nav_sat_count(const uint8_t *payload, size_t len);

/** Decode satellite idx of a UBX-NAV-SAT payload. @return 0 or -EINVAL. */
int ubx_nav_sat_get(const uint8_t *payload, size_t len, unsigned int idx, struct ubx_sat *out);

/** Result of an UBX-ACK frame: 1 for ACK, 0 for NAK, -EINVAL if msg is no ACK for (cls, id). */
int ubx_ack_result(const struct ubx_msg *msg, uint8_t cls, uint8_t id);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_UBX_H_ */
