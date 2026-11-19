/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_secure.h
 * @brief End-to-end envelope of SENSEI: key derivation, seal, open, replay window.
 *
 * C99, no allocation, no Zephyr. The cipher comes in through struct sensei_secure_crypto (backends:
 * Zephyr with the SENSEI SDK crypto library, OpenSSL 3 for host tests). See README.md for the key
 * derivation and the byte layout.
 *
 *   envelope = flags (1) | kind (1) | [node_id (8)] | key_id (4) | [sender (4)] | counter (4)
 *              | AES-GCM ciphertext | tag (16)
 *
 * The sender field is only in backend to node envelopes. Every sealing host has its own sender id, so
 * two hosts never use one nonce. The receiving node keeps one replay window per sender.
 *
 * Generation. Generation 0 is the envelope as it always was. From generation 1 on (forward secrecy, sensei_ratchet.h)
 * the kind byte also carries the generation: 0x80 | (generation mod 4) << 4 | kind. The key of such an envelope is not
 * the session key but comes from the keyring hook (struct sensei_secure_keyring): the message key of the epoch of the
 * counter for uplinks, the downlink key of the generation for downlinks. Read the kind with sensei_secure_kind(), never
 * as buf[1].
 */

#ifndef SENSEI_SECURE_H_
#define SENSEI_SECURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version 3. The version field is 2 bits and only odd values are used: bit 2 of the first byte is always set,
 * so the first byte of an envelope is 0xAC ... 0xAF and never 0xA0 or 0xA8. A plain Settings patch starts with
 * those two bytes when its first field is 20, 21, 36, 37, 52 or 53 (all varints). */
#define SENSEI_SECURE_VERSION 3u
#define SENSEI_SECURE_MARKER 0xA0u /* high nibble of the first byte */
#define SENSEI_SECURE_PUB_SIZE 65u
#define SENSEI_SECURE_PRIV_SIZE 32u
#define SENSEI_SECURE_KEY_SIZE 16u
#define SENSEI_SECURE_TAG_SIZE 16u
#define SENSEI_SECURE_SENDER_SIZE 4u
#define SENSEI_SECURE_HEADER 10u                                                     /* node to backend */
#define SENSEI_SECURE_HEADER_NODE 18u                                                /* node to backend with node id */
#define SENSEI_SECURE_HEADER_DOWN (SENSEI_SECURE_HEADER + SENSEI_SECURE_SENDER_SIZE) /* backend to node */
#define SENSEI_SECURE_HEADER_MAX (SENSEI_SECURE_HEADER_NODE + SENSEI_SECURE_SENDER_SIZE)
#define SENSEI_SECURE_OVERHEAD (SENSEI_SECURE_HEADER + SENSEI_SECURE_TAG_SIZE)           /* 26 */
#define SENSEI_SECURE_OVERHEAD_NODE (SENSEI_SECURE_HEADER_NODE + SENSEI_SECURE_TAG_SIZE) /* 34 */
#define SENSEI_SECURE_OVERHEAD_DOWN (SENSEI_SECURE_HEADER_DOWN + SENSEI_SECURE_TAG_SIZE) /* 30 */
#define SENSEI_SECURE_OVERHEAD_MAX (SENSEI_SECURE_HEADER_MAX + SENSEI_SECURE_TAG_SIZE)   /* 38 */
#define SENSEI_SECURE_WINDOW 64u
/* Senders a receiver tracks (a node: collector, GUIs). A new sender is refused when the table is full. */
#ifndef SENSEI_SECURE_MAX_SENDERS
#define SENSEI_SECURE_MAX_SENDERS 8u
#endif

/* Kind byte (byte 1). Generation 0: the kind, 1 ... 4. Generation 1 and later: SENSEI_SECURE_KIND_RATCHET |
 * (generation mod 4) << SENSEI_SECURE_GEN_SHIFT | kind. SENSEI_SECURE_KIND_RESERVED must be 0: such an envelope is
 * refused. */
#define SENSEI_SECURE_KIND_MASK 0x0Fu
#define SENSEI_SECURE_KIND_RATCHET 0x80u
#define SENSEI_SECURE_KIND_RESERVED 0x40u
#define SENSEI_SECURE_GEN_SHIFT 4u
#define SENSEI_SECURE_GEN_MASK 0x30u

/* Message kinds (in the associated data, without the generation bits). */
enum sensei_secure_kind {
  SENSEI_SECURE_KIND_UPLINK = 1,       /* node to backend: encoded Uplink */
  SENSEI_SECURE_KIND_DOWNLINK = 2,     /* backend to node: encoded Downlink */
  SENSEI_SECURE_KIND_NODE_TO_HOST = 3, /* node to host: encoded NodeToHost data message */
  SENSEI_SECURE_KIND_SETTINGS = 4,     /* backend to node: Settings patch for the BLE characteristic */
};

enum sensei_secure_role {
  SENSEI_SECURE_ROLE_NODE = 0,
  SENSEI_SECURE_ROLE_BACKEND = 1,
};

/* Errors (negative). */
enum {
  SENSEI_SECURE_ERR_FORMAT = -1,  /* not an envelope, wrong version, too short */
  SENSEI_SECURE_ERR_KEY_ID = -2,  /* envelope belongs to another key */
  SENSEI_SECURE_ERR_AUTH = -3,    /* tag does not verify */
  SENSEI_SECURE_ERR_REPLAY = -4,  /* counter seen before or too old */
  SENSEI_SECURE_ERR_SPACE = -5,   /* output buffer too small */
  SENSEI_SECURE_ERR_CRYPTO = -6,  /* backend failure */
  SENSEI_SECURE_ERR_STATE = -7,   /* session not open, counter exhausted, bad argument */
  SENSEI_SECURE_ERR_NODE = -8,    /* envelope carries another node id */
  SENSEI_SECURE_ERR_KIND = -9,    /* kind not allowed here */
  SENSEI_SECURE_ERR_STORE = -10,  /* the counter could not be persisted */
  SENSEI_SECURE_ERR_SENDER = -11, /* sender table full */
  SENSEI_SECURE_ERR_GEN = -12,    /* no key for the generation of the envelope (or no keyring) */
  SENSEI_SECURE_ERR_EPOCH = -13,  /* epoch before the start of the chain, or behind the part of it that is kept */
  SENSEI_SECURE_ERR_GAP = -14,    /* the chain would have to be walked further than the limit (MAX_SKIP epochs) */
  SENSEI_SECURE_ERR_DH = -15,     /* X25519 result is all zero (low-order public key) */
};

/** Opaque key handle of the backend (a PSA key id, or a pointer in the host backend). 0 is invalid.
 */
typedef uintptr_t sensei_secure_key_t;

/** Cipher operations. All return 0 on success, nonzero on failure (verify failure included). */
struct sensei_secure_crypto {
  void *ctx;
  int (*random)(void *ctx, uint8_t *out, size_t len);
  /** Import a P-256 private scalar (32 B big endian). Fails if it is not a valid scalar. */
  int (*ecdh_import)(void *ctx, const uint8_t *priv, size_t len, sensei_secure_key_t *key);
  /** Public key as 65 B uncompressed point. */
  int (*ecdh_public)(void *ctx, sensei_secure_key_t key, uint8_t *pub);
  /** Raw shared secret (32 B). */
  int (*ecdh_shared)(void *ctx, sensei_secure_key_t key, const uint8_t *peer_pub, uint8_t *secret);
  int (*hkdf)(void *ctx, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info,
              size_t info_len, uint8_t *okm, size_t okm_len);
  /** AES-128 key for AES-GCM. */
  int (*aead_import)(void *ctx, const uint8_t *key, sensei_secure_key_t *handle);
  int (*key_destroy)(void *ctx, sensei_secure_key_t key);
  /** Output is ciphertext followed by the 16 B tag. 12 B nonce. */
  int (*aead_encrypt)(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
  /** Input is ciphertext followed by the tag; no plaintext is released if the tag fails. */
  int (*aead_decrypt)(void *ctx, sensei_secure_key_t key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
  /* Forward secrecy (sensei_ratchet.h). A backend that has no ratchet leaves these NULL. */
  /** HMAC-SHA256 (RFC 2104). key_len is not 0. */
  int (*hmac_sha256)(void *ctx, const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]);
  /** New X25519 key: 32 random bytes as private key and its public key (RFC 7748). */
  int (*x25519_keygen)(void *ctx, uint8_t priv[32], uint8_t pub[32]);
  /** X25519(priv, peer) of RFC 7748: priv is clamped inside, the top bit of peer is ignored. Any 32 bytes are valid
   * inputs. The result may be all zero (a peer of low order) or the backend may refuse it with a nonzero return;
   * the caller treats both as failure. */
  int (*x25519)(void *ctx, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]);
};

struct sensei_secure_keys {
  uint8_t k_up[SENSEI_SECURE_KEY_SIZE];   /* node to backend */
  uint8_t k_down[SENSEI_SECURE_KEY_SIZE]; /* backend to node */
  uint32_t key_id;                        /* never 0 */
};

/**
 * Create a fresh private scalar: random 32 B that the backend accepts as P-256 key. The caller
 * stores it (the private key never leaves the node). Returns 0 or SENSEI_SECURE_ERR_CRYPTO.
 */
int sensei_secure_keygen(const struct sensei_secure_crypto *c, uint8_t priv[SENSEI_SECURE_PRIV_SIZE],
                         uint8_t pub[SENSEI_SECURE_PUB_SIZE]);

/**
 * True if priv is a valid P-256 private scalar: not 0 and below the group order. No cipher backend needed, so it also
 * tells a damaged stored key from a crypto failure of the backend.
 */
bool sensei_secure_priv_valid(const uint8_t priv[SENSEI_SECURE_PRIV_SIZE]);

/**
 * ECDH with the peer and HKDF-SHA256. own is the imported private key of role. The node passes
 * its own and the backend public key, the backend the same two (node_pub, backend_pub) in this
 * order.
 */
int sensei_secure_derive(const struct sensei_secure_crypto *c, sensei_secure_key_t own, enum sensei_secure_role role,
                         const uint8_t node_pub[SENSEI_SECURE_PUB_SIZE],
                         const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE], struct sensei_secure_keys *out);

/** Highest counter received and a bitmap: bit i set means counter (highest - i) was seen. */
struct sensei_secure_replay {
  uint32_t highest;
  uint64_t bitmap;
};

/** 0 if the counter is new, SENSEI_SECURE_ERR_REPLAY if seen, too old or 0. */
int sensei_secure_replay_check(const struct sensei_secure_replay *r, uint32_t counter);
void sensei_secure_replay_update(struct sensei_secure_replay *r, uint32_t counter);

/** Persisted state of one sender: its highest counter received. */
struct sensei_secure_rx_state {
  uint32_t sender;
  uint32_t highest;
};

/** Where the counters go. Both callbacks may be NULL (no storage: tests, host tools). */
struct sensei_secure_store {
  /** Store the new high-water mark of sent counters. Return 0 on success. */
  int (*tx)(void *user, uint32_t reserved);
  /** Store the highest counter received from a sender (called when it grows). Return 0 on success; on failure
   * the message is refused and the window does not change. */
  int (*rx)(void *user, uint32_t sender, uint32_t highest);
  void *user;
};

/**
 * Keys of the generations from 1 on. Optional, set with sensei_secure_set_keyring(). The key buffers hold 16 B
 * (AES-128-GCM); the session imports one for one message and wipes it.
 */
struct sensei_secure_keyring {
  void *user;
  /**
   * Called by seal after it has taken the counter, before it encrypts. Set *gen to the generation to seal in. 0: the
   * session key is used (key is not written). 1 or more: write the key of the counter (for a node: the message key of
   * the epoch of the counter, for a backend: the downlink key of the generation); the envelope gets the ratchet bits
   * of *gen mod 4. A nonzero return fails the seal with SENSEI_SECURE_ERR_GEN; nothing is sent and the counter is not
   * used (the next seal takes it again).
   */
  int (*tx_key)(void *user, uint32_t counter, uint32_t *gen, uint8_t key[SENSEI_SECURE_KEY_SIZE]);
  /**
   * Called by open for a header that passed the checks (kind, key id, node id, replay window), before it decrypts.
   * ratchet false: a generation 0 envelope, gen_bits is 0; true: generation gen_bits mod 4. Return 0 and write the key
   * to use; or, for a generation 0 envelope only, a positive value to use the session key; or a negative
   * SENSEI_SECURE_ERR_* code to refuse the envelope (for example ERR_GEN: no such generation, or ERR_EPOCH). Without
   * this hook generation 0 uses the session key and every ratchet envelope is refused with ERR_GEN. The hook must not
   * change what it keeps (a chain, a generation) for an envelope that is not yet authenticated: it is called before the
   * tag is checked.
   */
  int (*rx_key)(void *user, bool ratchet, unsigned gen_bits, uint32_t counter, uint8_t key[SENSEI_SECURE_KEY_SIZE]);
};

struct sensei_secure_peer {
  uint32_t sender;
  struct sensei_secure_replay rx;
};

struct sensei_secure {
  const struct sensei_secure_crypto *c;
  enum sensei_secure_role role;
  bool open;
  uint32_t key_id;
  sensei_secure_key_t k_tx, k_rx;
  uint32_t tx_sender;   /* sender id in the envelopes this side seals (0 on the node side: no field) */
  uint32_t tx_last;     /* last counter used */
  uint32_t tx_reserved; /* persisted high-water mark: all counters used are <= this */
  uint32_t ctr_step;    /* counters reserved per persist */
  struct sensei_secure_store store;
  struct sensei_secure_keyring ring;
  struct sensei_secure_peer peers[SENSEI_SECURE_MAX_SENDERS]; /* replay windows by sender */
  size_t n_peers;
};

/**
 * Open a session. All state is cleared first; nothing is read from *s.
 * - tx_sender: sender id of the backend (not 0); 0 for the node.
 * - tx_reserved: the persisted high-water mark of sent counters. Sending resumes above it. It belongs to the
 *   sender, not to the key: never lower it when a key is replaced.
 * - rx, n_rx: persisted highest counter per sender. After a reboot a window counts as fully seen, so nothing at
 *   or below it is accepted again.
 * - store: persistence callbacks, copied; may be NULL.
 */
int sensei_secure_init(struct sensei_secure *s, const struct sensei_secure_crypto *c, enum sensei_secure_role role,
                       const struct sensei_secure_keys *keys, uint32_t tx_sender, uint32_t tx_reserved,
                       uint32_t ctr_step, const struct sensei_secure_rx_state *rx, size_t n_rx,
                       const struct sensei_secure_store *store);
void sensei_secure_close(struct sensei_secure *s);

/** Set (or, with NULL, remove) the keyring of the generations from 1 on. Copied. Call it after sensei_secure_init(),
 * which clears the session. */
void sensei_secure_set_keyring(struct sensei_secure *s, const struct sensei_secure_keyring *ring);

/**
 * Move the sending counter up: the next seal uses at least next. Never lowers it (a counter is never reused). Used for
 * an epoch jump: next = the first counter of a new epoch (a multiple of 64) or of the first epoch of a new generation.
 * The persisted mark is raised by the next seal (store.tx). Returns SENSEI_SECURE_ERR_STATE if next is 0 or the session
 * is not open, 0 otherwise (also when the counter is already above next).
 */
int sensei_secure_tx_jump(struct sensei_secure *s, uint32_t next);

/** Highest counter received from a sender (0 if none). */
uint32_t sensei_secure_rx_highest(const struct sensei_secure *s, uint32_t sender);

/** True if the buffer looks like an envelope of this version: marker 0xA and version bits 3..2 (a plain Uplink
 * or Downlink never starts with 0xAx; a plain Settings patch may start with 0xA0 or 0xA8, never 0xAC ... 0xAF).
 */
static inline bool sensei_secure_is_sealed(const uint8_t *buf, size_t len) {
  return len >= SENSEI_SECURE_OVERHEAD && (buf[0] & 0xFCu) == (SENSEI_SECURE_MARKER | (SENSEI_SECURE_VERSION << 2));
}

/** Message kind of an envelope (SENSEI_SECURE_KIND_*): the kind byte without the generation bits. The buffer must
 * be an envelope (sensei_secure_is_sealed()). Compare this and never buf[1], which has the ratchet bits set from
 * generation 1 on. */
static inline unsigned sensei_secure_kind(const uint8_t *buf) { return buf[1] & SENSEI_SECURE_KIND_MASK; }

/** True if the kind byte says generation 1 or later. */
static inline bool sensei_secure_is_ratchet(const uint8_t *buf) { return (buf[1] & SENSEI_SECURE_KIND_RATCHET) != 0; }

/** Generation mod 4 of an envelope (0 for generation 0). */
static inline unsigned sensei_secure_gen_bits(const uint8_t *buf) {
  return sensei_secure_is_ratchet(buf) ? (unsigned)((buf[1] & SENSEI_SECURE_GEN_MASK) >> SENSEI_SECURE_GEN_SHIFT) : 0u;
}

/** Bytes an envelope of len plaintext bytes takes. to_node: backend to node (has the sender field). */
static inline size_t sensei_secure_sealed_size(size_t len, bool with_node_id, bool to_node) {
  return len + (with_node_id ? SENSEI_SECURE_OVERHEAD_NODE : SENSEI_SECURE_OVERHEAD) +
         (to_node ? SENSEI_SECURE_SENDER_SIZE : 0u);
}

/**
 * Seal pt of kind. The kind must go in this session's sending direction. node_id may be NULL
 * (omitted). Returns the envelope length, or a negative error. Uses and persists the next counter.
 */
int sensei_secure_seal(struct sensei_secure *s, enum sensei_secure_kind kind, const uint64_t *node_id,
                       const uint8_t *pt, size_t pt_len, uint8_t *out, size_t out_cap);

/**
 * Open an envelope. want: expected kind, or 0 for any kind of this direction. own_id: if not NULL
 * and the envelope carries a node id, it must match. Returns 0 and sets *out_len and *kind, or a
 * negative error. Nothing changes on failure (no plaintext is left in out); on success the counter is
 * recorded. If the counter cannot be stored (store.rx fails) the message is refused with ERR_STORE.
 */
int sensei_secure_open(struct sensei_secure *s, enum sensei_secure_kind want, const uint64_t *own_id, const uint8_t *in,
                       size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, enum sensei_secure_kind *kind);

/** Key id of an envelope without opening it (0 if it is not one). Lets a backend pick the session.
 */
uint32_t sensei_secure_peek_key_id(const uint8_t *buf, size_t len);

/** The header of an envelope, read without opening it (nothing is authenticated). */
struct sensei_secure_info {
  unsigned kind;     /* SENSEI_SECURE_KIND_* */
  bool to_node;      /* backend to node */
  bool ratchet;      /* generation 1 or later */
  unsigned gen_bits; /* generation mod 4 (0 if not ratchet) */
  bool has_node_id;
  uint64_t node_id;
  uint32_t key_id;
  uint32_t sender; /* 0 for node to backend */
  uint32_t counter;
};

/** Parse the header. SENSEI_SECURE_ERR_FORMAT if it is not an envelope of this version, is too short, has the reserved
 * kind bit or generation bits without the ratchet bit set, or the kind does not fit the direction. */
int sensei_secure_peek(const uint8_t *buf, size_t len, struct sensei_secure_info *info);

/* Backends. */
#ifdef SENSEI_SECURE_BACKEND_OPENSSL
const struct sensei_secure_crypto *sensei_secure_crypto_openssl(void);
#endif
#ifdef SENSEI_SECURE_BACKEND_ZEPHYR
/** Calls sensei_crypto_init() once. */
const struct sensei_secure_crypto *sensei_secure_crypto_zephyr(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_SECURE_H_ */
