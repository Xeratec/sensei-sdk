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
 *   envelope = flags (1) | kind (1) | [node_id (8)] | key_id (4) | counter (4) | AES-GCM ciphertext
 * | tag (16)
 */

#ifndef SENSEI_SECURE_H_
#define SENSEI_SECURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_SECURE_VERSION 1u
#define SENSEI_SECURE_MARKER 0xA0u /* high nibble of the first byte */
#define SENSEI_SECURE_PUB_SIZE 65u
#define SENSEI_SECURE_PRIV_SIZE 32u
#define SENSEI_SECURE_KEY_SIZE 16u
#define SENSEI_SECURE_TAG_SIZE 16u
#define SENSEI_SECURE_HEADER 10u
#define SENSEI_SECURE_HEADER_NODE 18u
#define SENSEI_SECURE_OVERHEAD (SENSEI_SECURE_HEADER + SENSEI_SECURE_TAG_SIZE)           /* 26 */
#define SENSEI_SECURE_OVERHEAD_NODE (SENSEI_SECURE_HEADER_NODE + SENSEI_SECURE_TAG_SIZE) /* 34 */
#define SENSEI_SECURE_WINDOW 64u

/* Message kinds (in the associated data). */
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
  SENSEI_SECURE_ERR_FORMAT = -1, /* not an envelope, wrong version, too short */
  SENSEI_SECURE_ERR_KEY_ID = -2, /* envelope belongs to another key */
  SENSEI_SECURE_ERR_AUTH = -3,   /* tag does not verify */
  SENSEI_SECURE_ERR_REPLAY = -4, /* counter seen before or too old */
  SENSEI_SECURE_ERR_SPACE = -5,  /* output buffer too small */
  SENSEI_SECURE_ERR_CRYPTO = -6, /* backend failure */
  SENSEI_SECURE_ERR_STATE = -7,  /* session not open, counter exhausted, bad argument */
  SENSEI_SECURE_ERR_NODE = -8,   /* envelope carries another node id */
  SENSEI_SECURE_ERR_KIND = -9,   /* kind not allowed here */
  SENSEI_SECURE_ERR_STORE = -10, /* the counter could not be persisted */
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

struct sensei_secure {
  const struct sensei_secure_crypto *c;
  enum sensei_secure_role role;
  bool open;
  uint32_t key_id;
  sensei_secure_key_t k_tx, k_rx;
  uint32_t tx_last;     /* last counter used */
  uint32_t tx_reserved; /* persisted high-water mark: all counters used are <= this */
  uint32_t ctr_step;    /* counters reserved per persist */
  struct sensei_secure_replay rx;
  /** Store the new high-water mark of sent counters. Return 0 on success. May be NULL. */
  int (*persist_tx)(void *user, uint32_t reserved);
  /** Store the highest received counter (called when it grows). May be NULL. */
  void (*persist_rx)(void *user, uint32_t highest);
  void *user;
};

/**
 * Open a session. tx_reserved: the persisted high-water mark (0 for a new key; sending resumes
 * above it). rx_highest: the persisted highest counter received; after a reboot the window is
 * treated as fully seen so nothing at or below it is accepted again (the backend's counters only
 * grow).
 */
int sensei_secure_init(struct sensei_secure *s, const struct sensei_secure_crypto *c, enum sensei_secure_role role,
                       const struct sensei_secure_keys *keys, uint32_t tx_reserved, uint32_t rx_highest,
                       uint32_t ctr_step);
void sensei_secure_close(struct sensei_secure *s);

/** True if the buffer looks like an envelope (a plain Uplink or Downlink never starts with 0xAx).
 */
static inline bool sensei_secure_is_sealed(const uint8_t *buf, size_t len) {
  return len >= SENSEI_SECURE_OVERHEAD && (buf[0] & 0xF0u) == SENSEI_SECURE_MARKER;
}

/** Bytes an envelope of len plaintext bytes takes. */
static inline size_t sensei_secure_sealed_size(size_t len, bool with_node_id) {
  return len + (with_node_id ? SENSEI_SECURE_OVERHEAD_NODE : SENSEI_SECURE_OVERHEAD);
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
 * negative error. Nothing changes on failure; on success the counter is recorded.
 */
int sensei_secure_open(struct sensei_secure *s, enum sensei_secure_kind want, const uint64_t *own_id, const uint8_t *in,
                       size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, enum sensei_secure_kind *kind);

/** Key id of an envelope without opening it (0 if it is not one). Lets a backend pick the session.
 */
uint32_t sensei_secure_peek_key_id(const uint8_t *buf, size_t len);

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
