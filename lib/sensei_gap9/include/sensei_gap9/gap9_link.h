/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file gap9_link.h
 * @brief Constants of the nRF5340 <-> GAP9 SPI link, shared by both sides. Plain C, no OS.
 *        The protocol is described in PROTOCOL.md.
 */

#ifndef SENSEI_GAP9_LINK_H_
#define SENSEI_GAP9_LINK_H_

#include "sensei_link/sensei_link.h"
#include "sensei_link/spi.h"

/* Frame types. 0x10 and 0x11 come from sensei_link.h. */
#define G9L_TYPE_JOB SENSEI_SPI_GAP9_JOB       /* nRF -> GAP9 (on MISO): the job message */
#define G9L_TYPE_RESULT SENSEI_SPI_GAP9_RESULT /* GAP9 -> nRF (on MOSI): the result message */
#define G9L_TYPE_HELLO 0x12u                   /* GAP9 -> nRF: Version, sent once after boot */
#define G9L_TYPE_POLL 0x13u                    /* GAP9 -> nRF: "send me your frame", header only */
#define G9L_TYPE_NONE 0x14u                    /* nRF -> GAP9: no job, header only */
#define G9L_TYPE_BUSFREE 0x15u                 /* GAP9 -> nRF: camera done, payload u32 seq LE */
#define G9L_TYPE_PEEK 0x16u                    /* GAP9 -> nRF: header-only look, consumes nothing */

/* Frame flags (sensei_link/spi.h): ERROR = "the previous frame you sent me was bad",
 * MORE = "more result frames of the same job follow" (JPEG chunks). */
#define G9L_FLAG_ERROR SENSEI_SPI_FLAG_ERROR
#define G9L_FLAG_MORE SENSEI_SPI_FLAG_MORE

/* Buffer sizes. TX: the largest job frame. RX is as long, so that the SPIS byte count tells how
 * much of the TX frame the master clocked out (GAP9 frames are at most 2082 B). */
#define G9L_HDR SENSEI_SPI_HEADER_SIZE
#define G9L_TX_CAP (SENSEI_SPI_HEADER_SIZE + SENSEI_SPI_MAX_PAYLOAD)
#define G9L_RX_CAP G9L_TX_CAP

/* Recovery: the SSBL (GAP9/ssbl) serves the update writer when no slot boots. Its HELLO has Version.major
 * G9L_RECOVERY_MAJOR, minor = the protocol of the recovery jobs (Gap9RecoveryJob), patch = the reason. The version
 * check of an app (codec gap9_major) never matches that major: an nRF5340 without recovery support refuses the HELLO
 * (-EPROTO) and powers GAP9 off. */
#define G9L_RECOVERY_MAJOR 0xFFFFu
#define G9L_RECOVERY_PROTO 1u
#define G9L_RECOVERY_BOTH_BAD 1u /* neither slot booted */
#define G9L_RECOVERY_FORCED 2u   /* the strap was high (an image that hangs before its HELLO) */

/* GAP9 pads (pi_pad_e numbers) of the link as the SSBL uses them in recovery; an app may use others. The slot line
 * selects the slot, the strap (pad 57, nRF5340 P1.06 EXT_RX_SPI) forces recovery. The strap is UNVERIFIED hardware. */
#define G9L_PAD_SCK 33u
#define G9L_PAD_MOSI 38u
#define G9L_PAD_MISO 39u
#define G9L_PAD_CS 34u
#define G9L_PAD_TRIGGER 58u
#define G9L_PAD_SLOT 56u
#define G9L_PAD_STRAP 57u
#define G9L_SPI_ITF 1u

/* SPI settings of the GAP9 master: mode 3, 8 bit, MSB first. */
#define G9L_SPI_HZ_DEFAULT 2000000u

/* GAP9 side timing. */
#define G9L_TURN_US 300u        /* wait after CS rises before the trigger line is read */
#define G9L_CS_SETUP_US 10u     /* CS low to first clock */
#define G9L_ARM_TIMEOUT_MS 1000 /* GAP9 waits this long for the trigger line */
#define G9L_RETRIES 3           /* tries per frame, GAP9 side and nRF side */

/* nRF side defaults (Kconfig overrides them). */
#define G9L_BOOT_TIMEOUT_MS 4000
#define G9L_BUSFREE_TIMEOUT_MS 4000
#define G9L_JOB_TIMEOUT_MS 8000
#define G9L_IDLE_OFF_MS 200
#define G9L_RESTART_OFF_MS 150 /* rail off time of a power cycle */

#endif /* SENSEI_GAP9_LINK_H_ */
