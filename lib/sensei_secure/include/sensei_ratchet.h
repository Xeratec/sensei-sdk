/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_ratchet.h
 * @brief Forward secrecy of the end-to-end envelope: root key, mix, uplink chain. C99, no allocation, no Zephyr; the
 *        cipher comes in through struct sensei_secure_crypto (hmac_sha256, x25519_keygen, x25519, hkdf).
 *
 * Key schedule (labels ASCII, integers u32 little endian, HKDF and HMAC with SHA-256). The Python twin is
 * sensei_sdk.ratchet; the vectors in tests/generated/vectors.h check that both agree.
 *
 *   R_0     = HKDF(ikm = k_up || k_down, salt = "SENSEI-E2E-root-v1", info = key_id, L = 32)
 *   mix to generation g+1 (ephemeral X25519 keys e_N of the node and e_C of the collector):
 *     dh    = X25519(e_N_priv, e_C_pub) = X25519(e_C_priv, e_N_pub)           all zero: refused
 *     th    = "SENSEI-RK-v1" || key_id || gen(g+1) || up_base || e_N_pub || e_C_pub          88 B
 *     okm   = HKDF(ikm = dh, salt = R_g, info = th, L = 80)
 *     R_{g+1} = okm[0:32]   CKu_{g+1,0} = okm[32:64]   k_down_{g+1} = okm[64:80]
 *   uplink chain, epoch(c) = c >> 6, index i = epoch(c) - up_base (below 0: refused):
 *     MK_i = HMAC(CK_i, 0x01)[0:16]   (AES-128-GCM key of the 64 counters of epoch i)
 *     CK_{i+1} = HMAC(CK_i, 0x02)
 *
 * This file has the primitives. The device key store (state, persistence, the keyring hook of the session) is built on
 * them; so is the collector's. Every function that fails leaves its output buffers wiped or unchanged as stated.
 */

#ifndef SENSEI_RATCHET_H_
#define SENSEI_RATCHET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_secure.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_RATCHET_KEY_SIZE 32u  /* X25519 private and public key, root key, chain key, dh */
#define SENSEI_RATCHET_MK_SIZE 16u   /* message key (AES-128-GCM) and downlink key */
#define SENSEI_RATCHET_TH_SIZE 88u   /* transcript of a mix */
#define SENSEI_RATCHET_EPOCH_BITS 6u /* an epoch is 64 counters, the size of the replay window */
#define SENSEI_RATCHET_EPOCH_SIZE 64u
/* Epochs one call walks along a chain (SENSEI_E2E_MAX_SKIP). More is refused with SENSEI_SECURE_ERR_GAP. */
#ifndef SENSEI_RATCHET_MAX_SKIP
#define SENSEI_RATCHET_MAX_SKIP (1u << 20)
#endif

/** Epoch of a counter. */
static inline uint32_t sensei_ratchet_epoch(uint32_t counter) { return counter >> SENSEI_RATCHET_EPOCH_BITS; }

/** True if the backend has what the ratchet needs (HMAC and X25519). */
static inline bool sensei_ratchet_available(const struct sensei_secure_crypto *c) {
  return c->hmac_sha256 != NULL && c->x25519 != NULL && c->x25519_keygen != NULL;
}

/** R_0 from the keys of generation 0. Both sides hold k_up, k_down and key_id. 0 or SENSEI_SECURE_ERR_CRYPTO. */
int sensei_ratchet_root0(const struct sensei_secure_crypto *c, const struct sensei_secure_keys *keys,
                         uint8_t root[SENSEI_RATCHET_KEY_SIZE]);

/** Public key of an X25519 private key, X25519(priv, 9). */
int sensei_ratchet_public(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_RATCHET_KEY_SIZE],
                          uint8_t pub[SENSEI_RATCHET_KEY_SIZE]);

/**
 * X25519(priv, peer). SENSEI_SECURE_ERR_DH if the result is all zero (a peer of low order), SENSEI_SECURE_ERR_CRYPTO if
 * the backend fails (it may refuse a low-order point itself). dh is wiped on failure.
 */
int sensei_ratchet_dh(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_RATCHET_KEY_SIZE],
                      const uint8_t peer[SENSEI_RATCHET_KEY_SIZE], uint8_t dh[SENSEI_RATCHET_KEY_SIZE]);

/** The transcript th of a mix to generation gen (the new one). */
void sensei_ratchet_transcript(uint32_t key_id, uint32_t gen, uint32_t up_base,
                               const uint8_t e_n_pub[SENSEI_RATCHET_KEY_SIZE],
                               const uint8_t e_c_pub[SENSEI_RATCHET_KEY_SIZE], uint8_t th[SENSEI_RATCHET_TH_SIZE]);

/** The keys of generation g+1. */
struct sensei_ratchet_mix {
  uint8_t root[SENSEI_RATCHET_KEY_SIZE];  /* R_{g+1}: salt of the next mix */
  uint8_t ck[SENSEI_RATCHET_KEY_SIZE];    /* CKu_{g+1,0}: head of the uplink chain */
  uint8_t k_down[SENSEI_RATCHET_MK_SIZE]; /* k_down_{g+1} */
};

/** The KDF step of a mix, from a given dh and transcript. SENSEI_SECURE_ERR_DH if dh is all zero. out is wiped on
 * failure. */
int sensei_ratchet_mix_dh(const struct sensei_secure_crypto *c, const uint8_t root[SENSEI_RATCHET_KEY_SIZE],
                          const uint8_t dh[SENSEI_RATCHET_KEY_SIZE], const uint8_t th[SENSEI_RATCHET_TH_SIZE],
                          struct sensei_ratchet_mix *out);

/**
 * A whole mix to generation gen. role: the side that calls; own_priv is its ephemeral private key (e_N for the
 * node, e_C for the collector), the other public key is taken from e_n_pub or e_c_pub. Both public keys go into the
 * transcript. The caller erases own_priv and root (R_g) after the switch. SENSEI_SECURE_ERR_DH or _CRYPTO on failure.
 */
int sensei_ratchet_mix(const struct sensei_secure_crypto *c, enum sensei_secure_role role,
                       const uint8_t own_priv[SENSEI_RATCHET_KEY_SIZE], const uint8_t root[SENSEI_RATCHET_KEY_SIZE],
                       uint32_t key_id, uint32_t gen, uint32_t up_base, const uint8_t e_n_pub[SENSEI_RATCHET_KEY_SIZE],
                       const uint8_t e_c_pub[SENSEI_RATCHET_KEY_SIZE], struct sensei_ratchet_mix *out);

/** MK = HMAC(ck, 0x01)[0:16]. */
int sensei_ratchet_mk(const struct sensei_secure_crypto *c, const uint8_t ck[SENSEI_RATCHET_KEY_SIZE],
                      uint8_t mk[SENSEI_RATCHET_MK_SIZE]);

/** CK' = HMAC(ck, 0x02). out may be ck (in place). */
int sensei_ratchet_next(const struct sensei_secure_crypto *c, const uint8_t ck[SENSEI_RATCHET_KEY_SIZE],
                        uint8_t out[SENSEI_RATCHET_KEY_SIZE]);

/** One uplink chain: ck is CK at index idx, index 0 is the epoch up_base. It only moves forward. */
struct sensei_ratchet_chain {
  uint32_t up_base;
  uint32_t idx;
  uint8_t ck[SENSEI_RATCHET_KEY_SIZE];
};

void sensei_ratchet_chain_init(struct sensei_ratchet_chain *ch, const uint8_t ck0[SENSEI_RATCHET_KEY_SIZE],
                               uint32_t up_base);
/** Wipe the chain key. */
void sensei_ratchet_chain_wipe(struct sensei_ratchet_chain *ch);

/**
 * Walk to index idx (the chain key of that epoch); the earlier keys are gone. SENSEI_SECURE_ERR_EPOCH if idx is behind
 * the chain, SENSEI_SECURE_ERR_GAP if more than max_skip steps, SENSEI_SECURE_ERR_CRYPTO on a backend failure. The
 * chain is unchanged on failure.
 */
int sensei_ratchet_chain_advance(const struct sensei_secure_crypto *c, struct sensei_ratchet_chain *ch, uint32_t idx,
                                 uint32_t max_skip);

/**
 * Message key of the epoch of counter. The epoch must not be before up_base and not behind the chain
 * (SENSEI_SECURE_ERR_EPOCH). With commit the chain moves to that epoch, without it is left as it was (the walk is done
 * on a copy: use this before the tag is checked). max_skip as for sensei_ratchet_chain_advance()
 * (SENSEI_SECURE_ERR_GAP). mk is wiped on failure.
 */
int sensei_ratchet_chain_key(const struct sensei_secure_crypto *c, struct sensei_ratchet_chain *ch, uint32_t counter,
                             bool commit, uint32_t max_skip, uint8_t mk[SENSEI_RATCHET_MK_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_RATCHET_H_ */
