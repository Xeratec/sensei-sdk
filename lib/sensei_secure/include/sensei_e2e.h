/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_e2e.h
 * @brief Device side of the end-to-end security: key pair, provisioning, session and the persisted
 *        counters (lib/sensei_secure/README.md). No Zephyr; storage and cipher come in through ops.
 *        The Zephyr settings storage is in sensei_e2e_settings.h.
 *
 * The private key is made once and never leaves the device. Provisioning stores the collector's
 * public key and derives the session. The counters belong to the device, not to the key: the sent
 * counter never goes back (a new key, or the old key again, goes on above the stored high-water
 * mark), and the highest counter of every sender of downlinks is kept (a table, refused when full).
 * Clearing the table leaves a floor: no downlink at or below the highest counter of the cleared
 * entries is accepted from any sender. Only a reset erases all of this, together with the key pair.
 */

#ifndef SENSEI_E2E_H
#define SENSEI_E2E_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_secure.h"

/** What is kept in flash. */
struct sensei_e2e_nv {
  uint8_t priv[SENSEI_SECURE_PRIV_SIZE];
  bool have_priv;
  uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE];
  bool have_backend;
  uint32_t tx_reserved;                                        /* high-water mark of sent counters, for every key */
  struct sensei_secure_rx_state rx[SENSEI_SECURE_MAX_SENDERS]; /* highest counter by sender */
  size_t n_rx;
  uint32_t rx_floor; /* downlinks with a counter at or below this are refused (0: no floor) */
  /* The stored key exists but cannot be used (a sealed record that does not open, or the sealing key is missing). Set
   * by the storage backend. sensei_e2e_init() then fails with -EACCES and leaves the stored key alone: a new key pair
   * would replace it. Only sensei_e2e_reset() starts over. */
  bool key_unreadable;
  /* The private key is a valid scalar that is stored sealed: it opened from the sealed record, or it was a plain key
   * that passed sensei_secure_priv_valid() and was sealed. If the public key cannot be derived from it, that is a
   * crypto error (for example no free key slot), not a damaged key: sensei_e2e_init() fails with -EIO and keeps the
   * stored record. A plain key that is not a valid scalar is damaged: it is not sealed and this flag stays false, so
   * sensei_e2e_init() replaces it. Set by the storage backend. */
  bool priv_sealed;
};

struct sensei_e2e_ops {
  int (*save_priv)(void *user, const uint8_t *priv);
  int (*save_backend)(void *user, const uint8_t *pub);
  int (*save_tx)(void *user, uint32_t reserved);
  /** Store the whole table of senders (n entries). Return 0 on success; a message is refused if
   * this fails. */
  int (*save_rx)(void *user, const struct sensei_secure_rx_state *table, size_t n);
  /** Delete everything stored: the collector key and the key first, the counters last (a counter must not come back
   * with a key that survived). Stop at the first error and return it; 0 on success. */
  int (*erase)(void *user);
  /** Store the floor of downlink counters. */
  int (*save_floor)(void *user, uint32_t floor);
};

struct sensei_e2e {
  const struct sensei_secure_crypto *c;
  const struct sensei_e2e_ops *ops;
  void *user;
  uint32_t ctr_step;
  struct sensei_e2e_nv nv;
  uint8_t node_pub[SENSEI_SECURE_PUB_SIZE];
  struct sensei_secure s;
  bool ready; /* a session is open */
};

/**
 * Load the state (nv: what was read from flash, may be all empty), make the key pair if there is
 * none and open the session if a collector key is stored. Returns 0 or a negative errno: -EACCES if
 * nv->key_unreadable is set, -EIO if the public key cannot be derived from a key that came from a sealed record
 * (nv->priv_sealed; nothing is replaced; a damaged plain key is replaced), or the error of the storage when the new key
 * pair cannot be stored (no key is kept then). A failed session leaves the module without a session (sensei_e2e_ready()
 * false).
 */
int sensei_e2e_init(struct sensei_e2e *e, const struct sensei_secure_crypto *c, const struct sensei_e2e_ops *ops,
                    void *user, const struct sensei_e2e_nv *nv, uint32_t ctr_step);

/** Store the collector's public key and derive the session. -EINVAL for a bad key (state
 * unchanged). */
int sensei_e2e_provision(struct sensei_e2e *e, const uint8_t *backend_pub, size_t len);

/** Forget the collector key and counters, delete the key pair and make a new one. Also the way out of a
 * key that could not be read (sensei_e2e_init() returned -EACCES; the struct is usable for this call). If the stored
 * records cannot be erased the reset is refused: the error of ops->erase is returned and the session and the state in
 * RAM stay as they were (some records may be gone from flash; call it again). */
int sensei_e2e_reset(struct sensei_e2e *e);

/** Senders of downlinks in the table, and its size. */
size_t sensei_e2e_senders_used(const struct sensei_e2e *e);
size_t sensei_e2e_senders_max(void);

/**
 * Empty the table of senders (a full table refuses every new sender). The keys and the sent counter
 * stay. The highest counter of the cleared entries becomes the floor (kept in flash, it only
 * rises): a downlink at or below it is refused (-EALREADY) whatever its sender, so recorded
 * downlinks cannot be replayed. A sender that goes on gets counters above the floor; a host that
 * takes a new sender must go on from the counter of the node, as the key store does. Returns 0, or
 * the error of the flash write (the table is then unchanged).
 */
int sensei_e2e_clear_senders(struct sensei_e2e *e);

bool sensei_e2e_ready(const struct sensei_e2e *e);
/** 0 without a session. */
uint32_t sensei_e2e_key_id(const struct sensei_e2e *e);

/** Seal kind (node to host or backend direction of the node). Returns the length or a negative
 * errno. */
int sensei_e2e_seal(struct sensei_e2e *e, enum sensei_secure_kind kind, const uint8_t *pt, size_t len, uint8_t *out,
                    size_t cap);

/**
 * Open a downlink or settings envelope. want: the kind that is allowed here. -EACCES (tag, key id),
 * -EALREADY (replay), -EBADMSG (format), -ENOSPC, -EIO, -ENOTCONN (no session).
 */
int sensei_e2e_open(struct sensei_e2e *e, enum sensei_secure_kind want, const uint8_t *in, size_t len, uint8_t *out,
                    size_t cap, size_t *out_len);

/** Map a SENSEI_SECURE_ERR_* code to a negative errno. */
int sensei_e2e_errno(int err);

#endif /* SENSEI_E2E_H */
