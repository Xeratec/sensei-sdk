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
 *
 * Forward secrecy (sensei_ratchet.h, design in README.md). After provisioning the device runs in generation 0, the
 * static keys. It then asks the collector for a mix (sensei_e2e_rekey_due() gives the request to put into an Uplink,
 * sensei_e2e_rekey_answer() takes the answer) and from generation 1 on seals uplinks under a chain of message keys
 * (one key per epoch of 64 counters) and accepts downlinks under the downlink key of its generation. The state of the
 * generation is one flash record (e2e_rat, ops.save_rat), written atomically. Counters are persisted in blocks of
 * whole epochs, together with the chain key at the start of the block.
 *
 * One thread uses a struct sensei_e2e (the hooks of the session keyring point into it: do not move it).
 */

#ifndef SENSEI_E2E_H
#define SENSEI_E2E_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_ratchet.h"
#include "sensei_secure.h"

/** Size of the plain ratchet record (e2e_rat, layout in sensei_e2e.c and README.md). */
#define SENSEI_E2E_RAT_SIZE 156u
/** Longest encoded RekeyRequest field that sensei_e2e_rekey_due() writes (field 14 of the Uplink, tag and length
 * included). */
#define SENSEI_E2E_REKEY_MAX 48u

/* Defaults of struct sensei_e2e_cfg. */
#ifndef SENSEI_E2E_REKEY_DAYS
#define SENSEI_E2E_REKEY_DAYS 7u /* mix period */
#endif
#ifndef SENSEI_E2E_REKEY_RETRY_H
#define SENSEI_E2E_REKEY_RETRY_H 6u /* a pending request is sent again after this many hours */
#endif
#ifndef SENSEI_E2E_EPOCH_MAX_S
#define SENSEI_E2E_EPOCH_MAX_S 86400u /* an epoch key is not used for longer than this */
#endif
/** A chain that is this many epochs long triggers a mix. */
#define SENSEI_E2E_REKEY_EPOCHS 4096u
/** A pending request is replaced by a new one (not sent again) once the node has sent this many epochs since the
 * request was made. An answer then needs a walk of at most this many epochs (about 2 to 5 s on the crypto queue of the
 * nRF5340, which the application work queue waits for; Kconfig SENSEI_SECURE_REKEY_STALE_EPOCHS sets it). A node that
 * seals more than this x 64 messages between a request and its answer does not mix until the traffic drops. */
#ifndef SENSEI_E2E_REKEY_STALE_EPOCHS
#define SENSEI_E2E_REKEY_STALE_EPOCHS 32768u
#endif
/** The counters are persisted in blocks of whole epochs: one epoch after a start, twice the last block while the
 * persists come less than SENSEI_E2E_PERSIST_FAST_S apart (streaming), up to this many epochs (65 536 counters), half
 * of it after a persist that comes later (a slow sender is back at one epoch after a few). Flash writes then stay at
 * about one per minute at any rate up to ~1000 messages a second. Trade-off: the chain key
 * in flash is the one of the first epoch of the block, so it opens the keys of the epochs of the block that were sent
 * already, at most this many epochs; a longer cap means fewer writes and more exposed messages. Must stay below
 * SENSEI_RATCHET_MAX_SKIP (a restart walks the chain from the stored epoch to the new counter). */
#ifndef SENSEI_E2E_BLOCK_EPOCHS_MAX
#define SENSEI_E2E_BLOCK_EPOCHS_MAX 1024u
#endif
#define SENSEI_E2E_PERSIST_FAST_S 60u

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
  /* The ratchet record e2e_rat (plain, SENSEI_E2E_RAT_SIZE bytes; a sealing backend has opened it). Set by the storage
   * backend. have_rat: there is a record that opened. rat_unreadable: there is a record that does not open (wrong size,
   * does not authenticate, wrong key): the state of the generation is lost. If the node is provisioned, it refuses to
   * seal and open until it is provisioned again (a fallback to generation 0 would be a downgrade). A record that opened
   * but belongs to another key id is old and is ignored. A generation 0 record (it only holds a pending request) is
   * dropped by a build without the ratchet (no X25519 or HMAC): such a build never asks for a mix and no key depends
   * on the record; a record of generation 1 or later is refused there (damaged). */
  uint8_t rat[SENSEI_E2E_RAT_SIZE];
  bool have_rat;
  bool rat_unreadable;
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
  /**
   * Store the ratchet record, SENSEI_E2E_RAT_SIZE bytes of plain data that belong to key_id, as one atomic write: after
   * a power cut the old or the new record is read, never a mix. rec NULL: delete the record. May be NULL (no storage:
   * the state is then lost at a restart; tests, host tools). Return 0 on success.
   */
  int (*save_rat)(void *user, uint32_t key_id, const uint8_t *rec, size_t len);
  /** Seconds of a monotonic clock (uptime). Time of the last persist, age of an epoch, age of the generation, retries.
   * NULL: the clock stands still at 0 (blocks grow to SENSEI_E2E_BLOCK_EPOCHS_MAX, no time-bound epochs, a pending
   * request is sent once per start, the age of a generation is 0). */
  uint32_t (*uptime_s)(void *user);
  /** Seconds since 1970 if the wall clock is set, else 0. For the start of a generation (SENSEI_E2E_REKEY_DAYS). NULL:
   * unknown; the period is then counted in uptime plus the age that every record of the generation carries (hours,
   * 16 bits), so a node that restarts often and has no wall clock still mixes about every SENSEI_E2E_REKEY_DAYS of
   * running time (the time since the last write of the record is lost at a restart, and the age is rounded up). A node
   * that is switched off (System OFF) does not count that time. If the clock appears after the switch, the start of
   * the generation is back-dated by the age. */
  uint32_t (*unix_s)(void *user);
};

/** Settings of the mix. sensei_e2e_configure() after sensei_e2e_init(); the defaults are the SENSEI_E2E_* macros above.
 */
struct sensei_e2e_cfg {
  bool rekey;           /**< ask for mixes (SENSEI_E2E_REKEY). Off: stays in the generation it is in, never asks. */
  uint32_t rekey_days;  /**< period; 0: no time trigger */
  uint32_t retry_h;     /**< a pending request is sent again after this many hours */
  uint32_t epoch_max_s; /**< an epoch key is not used longer than this (generation 1 and later); 0: no limit */
};

/** The ratchet state of the device. */
struct sensei_e2e_rat {
  uint32_t gen;            /**< 0: static keys */
  uint32_t gen_start_unix; /**< 0: unknown */
  uint32_t gen_start_up;   /**< uptime at the start of the generation (RAM only; may lie before the boot: the stored age
                              is taken off at the load) */
  uint8_t root[SENSEI_RATCHET_KEY_SIZE]; /**< R_gen: gen 0: R_0 */
  uint8_t k_down[SENSEI_RATCHET_MK_SIZE];
  struct sensei_ratchet_chain up; /**< uplink chain, generation 1 and later */
  bool prev;                      /**< the previous generation is still accepted for downlinks */
  uint8_t
      prev_k_down[SENSEI_RATCHET_MK_SIZE]; /**< generation gen-1, only meaningful from gen 2 (gen 1: the static key) */
  bool pending;                            /**< a request is out */
  uint32_t pend_up_base;
  uint8_t pend_priv[SENSEI_RATCHET_KEY_SIZE];
  uint8_t pend_pub[SENSEI_RATCHET_KEY_SIZE]; /**< RAM only, from pend_priv */
};

/** What `sensei e2e status` shows. */
struct sensei_e2e_status {
  uint32_t gen;
  bool pending; /**< a request is waiting for its answer */
  bool prev;    /**< the previous generation is still accepted for downlinks */
  bool damaged; /**< the ratchet state is lost or the provisioning stopped half way: sealing is refused until the node
                   is provisioned again */
  uint32_t up_base; /**< first epoch of the chain of this generation (generation 1 and later) */
  uint32_t up_idx;  /**< index of the epoch of the last counter used */
  uint32_t rekeys;  /**< switches since start */
  uint32_t refused; /**< seals and opens refused because of the damaged state, since start */
};

struct sensei_e2e {
  const struct sensei_secure_crypto *c;
  const struct sensei_e2e_ops *ops;
  void *user;
  uint32_t ctr_step; /* proposal of the secure layer; the blocks are whole epochs (see sensei_e2e_init()) */
  struct sensei_e2e_nv nv;
  uint8_t node_pub[SENSEI_SECURE_PUB_SIZE];
  struct sensei_secure s;
  bool ready; /* a session is open */
  struct sensei_e2e_cfg cfg;
  struct sensei_e2e_rat r;
  bool damaged;    /* the ratchet state is lost, or a provisioning stopped half way: refuse to seal and open */
  bool dirty;      /* a reset stopped half way (the erase failed): provisioning is refused until a reset succeeds */
  bool rat_stored; /* a record e2e_rat may be in flash */
  bool flash_idx_valid;
  uint32_t flash_idx; /* chain index of the up_ck in flash */
  bool persist_seen;
  uint32_t persist_t;    /* uptime of the last block persist */
  uint32_t block_epochs; /* epochs of the last block (1 .. SENSEI_E2E_BLOCK_EPOCHS_MAX) */
  bool sent_valid;
  uint32_t sent_t;    /* uptime of the last request sent */
  uint32_t epoch_cur; /* epoch of the last counter sealed in this run (UINT32_MAX: none) */
  uint32_t epoch_t0;  /* uptime when the epoch started */
  uint32_t rekeys, refused;
};

/**
 * Load the state (nv: what was read from flash, may be all empty), make the key pair if there is
 * none and open the session if a collector key is stored. ctr_step is only a proposal now: the counters are persisted
 * in blocks of whole epochs (one, growing to SENSEI_E2E_BLOCK_EPOCHS_MAX while the persists come fast), see the README.
 * nv is not const: the secrets of the caller's copy (nv->priv and nv->rat, with the ratchet secrets R_g, the chain key,
 * the downlink keys and the pending key) are wiped before the call returns, whether it succeeded or not, because the
 * module keeps its own copy. The flags (have_priv, have_backend, ...) and the counters stay; to initialize again, read
 * the flash again. Returns 0 or a negative errno:
 * -EACCES if nv->key_unreadable is set, -EIO if the public key cannot be derived from a key that came from a sealed
 * record (nv->priv_sealed; nothing is replaced; a damaged plain key is replaced), or the error of the storage when the
 * new key pair cannot be stored (no key is kept then). A failed session leaves the module without a session
 * (sensei_e2e_ready() false).
 */
int sensei_e2e_init(struct sensei_e2e *e, const struct sensei_secure_crypto *c, const struct sensei_e2e_ops *ops,
                    void *user, struct sensei_e2e_nv *nv, uint32_t ctr_step);

/**
 * Store the collector's public key and derive the session: the node is then in generation 0 with new static keys and
 * asks for a mix at the next uplink. The ratchet record is deleted; the counters and windows stay (the sent counter
 * goes on above the persisted mark). Also the way out of a damaged ratchet state. -EINVAL for a bad key (state
 * unchanged), -EBUSY after a reset that failed half way (call sensei_e2e_reset() again first), the error of a write
 * (nothing changed; or, if only the ratchet record could not be deleted, the node refuses to seal and open until
 * the call is repeated).
 */
int sensei_e2e_provision(struct sensei_e2e *e, const uint8_t *backend_pub, size_t len);

/** Forget the collector key and counters, delete the key pair and make a new one. Also the way out of a
 * key that could not be read (sensei_e2e_init() returned -EACCES; the struct is usable for this call). If the stored
 * records cannot be erased the reset is refused: the error of ops->erase is returned and the session and the state in
 * RAM stay as they were (some records may be gone from flash; call it again). Until a reset succeeds,
 * sensei_e2e_provision() is refused (-EBUSY): it would store a collector key for a key pair that is gone from flash. */
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

/** Settings of the mix (defaults: all rekey on if the backend has X25519 and HMAC, SENSEI_E2E_REKEY_DAYS, _RETRY_H,
 * SENSEI_E2E_EPOCH_MAX_S). Call it after sensei_e2e_init(), once. */
void sensei_e2e_configure(struct sensei_e2e *e, const struct sensei_e2e_cfg *cfg);

/** Generation of the keys in use: 0 (static keys), 1 and later after a mix. */
uint32_t sensei_e2e_gen(const struct sensei_e2e *e);

void sensei_e2e_status(const struct sensei_e2e *e, struct sensei_e2e_status *st);

/**
 * The request for a mix, to append to an encoded Uplink before it is sealed: call it for every Uplink of kind
 * SENSEI_SECURE_KIND_UPLINK (not for host data, kind 3). It starts a mix when one is due (generation 0 after
 * provisioning, SENSEI_E2E_REKEY_DAYS since the generation started, a chain of SENSEI_E2E_REKEY_EPOCHS epochs, or
 * sensei_e2e_rekey_now()): makes the ephemeral key, drops the previous downlink key and stores the state BEFORE it
 * returns the request. A pending request is returned again after retry_h hours (and at the first call after a start).
 * Writes the encoded field 14 of the Uplink (tag 0x72, length, RekeyRequest{gen = 1, pub = 2, up_base = 3}) to out,
 * appended messages merge in protobuf; returns its length (at most SENSEI_E2E_REKEY_MAX), 0 if there is nothing to
 * send, or a negative errno: -ENOSPC (cap too small, nothing marked as sent), -ENOTCONN, -EPERM (damaged), -EIO, or the
 * error of the write that stores the new state (the state is then as it was). If the Uplink is not sealed or sent after
 * all, call sensei_e2e_rekey_unsent() so the request goes out with the next one.
 */
int sensei_e2e_rekey_due(struct sensei_e2e *e, uint8_t *out, size_t cap);

/** The request returned by sensei_e2e_rekey_due() was not sent: send it with the next Uplink. */
void sensei_e2e_rekey_unsent(struct sensei_e2e *e);

/**
 * The answer of the collector (RekeyAnswer{gen, pub, up_base}, decoded, from a downlink that sensei_e2e_open()
 * accepted). Only an answer to the pending request counts: gen = generation + 1, up_base equal to the request, pub 32
 * bytes and not of low order. Then the node computes the new generation, writes the state (one atomic record) and
 * switches: every uplink from then on is sealed in the new generation. The first counter of the generation is
 * max(last + 1, 64 * up_base), moved up to the next epoch boundary when the first uplink is sealed (a generation starts
 * a new epoch; up_base lies above the stored mark, so it is 64 * up_base unless the node sent past it while the request
 * was out); the ephemeral key, R_g and the chain of generation g are erased. Returns 0 (switched); -EALREADY (no
 * request pending, or the generation is not new: a repeated answer is ignored); -EBADMSG (gen, up_base or length do not
 * match); -EACCES (pub of low order); or the error of the write (nothing changed, the request stays pending: a repeated
 * answer tries again). The previous generation's downlink key is kept until the next request.
 */
int sensei_e2e_rekey_answer(struct sensei_e2e *e, uint32_t gen, const uint8_t *pub, size_t pub_len, uint32_t up_base);

/**
 * Start a mix now (a sealed Downlink.rekey_now): like sensei_e2e_rekey_due() it creates and stores the request; the
 * next due() call sends it. -EALREADY if one is pending (it is sent again at the next Uplink), -ENOTSUP if the node
 * cannot mix (no X25519 in the backend, cfg.rekey off), -EPERM, -ENOTCONN, or the error of the write.
 */
int sensei_e2e_rekey_now(struct sensei_e2e *e);

bool sensei_e2e_ready(const struct sensei_e2e *e);
/** 0 without a session. */
uint32_t sensei_e2e_key_id(const struct sensei_e2e *e);

/** Seal kind (node to host or backend direction of the node). Returns the length or a negative
 * errno; -EPERM if the ratchet state is damaged (counted, see sensei_e2e_status()); -EPROTO if the node has no key for
 * the counter (the key hook of the session failed: SENSEI_SECURE_ERR_TXKEY; nothing was sent, the counter is not used,
 * a bug or a crypto failure, not a refusal of the peer). From generation 1 on the counter
 * may jump forward (a new epoch after a restart, an epoch older than epoch_max_s, the first epoch of a generation). */
int sensei_e2e_seal(struct sensei_e2e *e, enum sensei_secure_kind kind, const uint8_t *pt, size_t len, uint8_t *out,
                    size_t cap);

/**
 * Open a downlink or settings envelope. want: the kind that is allowed here. -EACCES (tag, key id, no key for the
 * generation of the envelope: from generation 1 on a generation 0 envelope is refused once the previous generation is
 * dropped), -EALREADY (replay), -EBADMSG (format), -ENOSPC, -EIO, -ENOTCONN (no session), -EPERM (damaged ratchet
 * state).
 */
int sensei_e2e_open(struct sensei_e2e *e, enum sensei_secure_kind want, const uint8_t *in, size_t len, uint8_t *out,
                    size_t cap, size_t *out_len);

/**
 * The key id that a node with private key priv gets for the collector key backend_pub (what sensei_secure_derive()
 * gives): for the storage backend, to tell which of two stored keys the ratchet record belongs to. 0, -EIO (crypto) or
 * -EINVAL (not a usable collector key).
 */
int sensei_e2e_key_id_of(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_SECURE_PRIV_SIZE],
                         const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE], uint32_t *key_id);

/** Map a SENSEI_SECURE_ERR_* code to a negative errno. */
int sensei_e2e_errno(int err);

#endif /* SENSEI_E2E_H */
