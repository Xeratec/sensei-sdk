/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#ifndef SENSEI_OTA_H_
#define SENSEI_OTA_H_

/**
 * @file sensei_ota.h
 * @brief Firmware update helpers for apps that run under MCUboot (CONFIG_SENSEI_OTA).
 *
 * - Trial boot: after an update the new image runs on trial. sensei_ota_init() watches it, confirms it
 *   when the app's check passes (and, by default, an SMP request came in on the image: BLE or USB update path
 *   works), and resets the node when it does not (MCUboot then reverts).
 * - SMP server: the Kconfig options select the mcumgr image and OS groups and the Bluetooth transport.
 * - SMP over a byte stream that carries another protocol too (USB CDC): sensei_smp_stream_*().
 *
 * See README.md.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sensei_ota_state {
  SENSEI_OTA_UNKNOWN = 0,   /**< state of the running image could not be read */
  SENSEI_OTA_CONFIRMED = 1, /**< the running image is permanent */
  SENSEI_OTA_TRIAL = 2,     /**< on trial: reverts at the next reset unless confirmed */
};

/** Health check of the running image. Called every few seconds during a trial; must not block.
 *  Return true when the app works. */
typedef bool (*sensei_ota_check_fn)(void);

/**
 * Start the trial supervision if the running image is not confirmed. Call it once, at the end of
 * the app start-up. With CONFIG_SENSEI_OTA_CONFIRM the image is confirmed when `check` (may be NULL)
 * has passed after CONFIG_SENSEI_OTA_CONFIRM_DELAY_S seconds and, with
 * CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK, an SMP request (or sensei_ota_link_seen()) came in on this image.
 * If it is still unconfirmed after CONFIG_SENSEI_OTA_TRIAL_TIMEOUT_S seconds, the node resets, so that
 * MCUboot reverts to the old image. A confirmation over SMP ends the trial as well.
 */
int sensei_ota_init(sensei_ota_check_fn check);

/**
 * Tell the trial supervision that the update path works. SMP requests are noticed by themselves
 * (CONFIG_SENSEI_OTA_CONFIRM_NEEDS_LINK selects the mcumgr hook). Call this only for an update path that is
 * not SMP: traffic of an app protocol on the same link does not show that SMP works. Any thread; harmless on
 * a confirmed image.
 */
void sensei_ota_link_seen(void);

/** State of the running image. */
enum sensei_ota_state sensei_ota_state(void);

/** Make the running image permanent now. 0 on success. */
int sensei_ota_confirm(void);

/** First 4 bytes of the SHA-256 of the running image (the image id of mcumgr), little endian; 0 if unknown. */
uint32_t sensei_ota_image_id(void);

#ifdef CONFIG_SENSEI_OTA_SMP_STREAM
/**
 * Output of the SMP stream transport: one mcumgr serial line (2 byte marker, base64, '\n'). Called on
 * the mcumgr work queue. Return 0 when the line is queued.
 */
typedef int (*sensei_smp_stream_tx_fn)(const uint8_t *line, size_t len, void *user);

/** Register the SMP stream transport. */
int sensei_smp_stream_init(sensei_smp_stream_tx_fn tx, void *user);

/**
 * Feed one received byte of a shared stream. The other protocol's frames must be ended by 0x00 and
 * must not start with 0x06 0x09 or 0x04 0x14 (the SMP line markers); then SMP lines can be told
 * apart from them. `pass` gets the bytes that belong to the other protocol, in order. Returns how
 * many (0, 1 or 2): SMP line bytes return 0, and a held first byte is returned together with the
 * next one.
 */
size_t sensei_smp_stream_feed(uint8_t byte, uint8_t pass[2]);

/** Call when the stream (re)starts: drops a half-received line. */
void sensei_smp_stream_reset(void);

/** The stream transport saw a complete SMP line less than `ms` ago (to hold back other traffic). */
bool sensei_smp_stream_active(uint32_t ms);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_OTA_H_ */
