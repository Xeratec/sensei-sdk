/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_ratchet.h"

#include <string.h>

#define MIX_OKM 80u

static const uint8_t root_salt[] = "SENSEI-E2E-root-v1"; /* the NUL is not part of the label */
static const uint8_t mix_label[] = "SENSEI-RK-v1";
#define ROOT_SALT_LEN (sizeof(root_salt) - 1)
#define MIX_LABEL_LEN (sizeof(mix_label) - 1)

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;

  while (n--) {
    *v++ = 0;
  }
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static bool all_zero(const uint8_t *p, size_t n) {
  uint8_t acc = 0;

  while (n--) {
    acc |= *p++;
  }
  return acc == 0;
}

int sensei_ratchet_root0(const struct sensei_secure_crypto *c, const struct sensei_secure_keys *keys,
                         uint8_t root[SENSEI_RATCHET_KEY_SIZE]) {
  uint8_t ikm[2 * SENSEI_SECURE_KEY_SIZE];
  uint8_t info[4];
  int err;

  memcpy(ikm, keys->k_up, SENSEI_SECURE_KEY_SIZE);
  memcpy(ikm + SENSEI_SECURE_KEY_SIZE, keys->k_down, SENSEI_SECURE_KEY_SIZE);
  put32(info, keys->key_id);
  err = c->hkdf(c->ctx, ikm, sizeof(ikm), root_salt, ROOT_SALT_LEN, info, sizeof(info), root, SENSEI_RATCHET_KEY_SIZE);
  wipe(ikm, sizeof(ikm));
  if (err) {
    wipe(root, SENSEI_RATCHET_KEY_SIZE);
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  return 0;
}

int sensei_ratchet_public(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_RATCHET_KEY_SIZE],
                          uint8_t pub[SENSEI_RATCHET_KEY_SIZE]) {
  uint8_t base[SENSEI_RATCHET_KEY_SIZE] = {9};

  return c->x25519(c->ctx, priv, base, pub) == 0 ? 0 : SENSEI_SECURE_ERR_CRYPTO;
}

int sensei_ratchet_dh(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_RATCHET_KEY_SIZE],
                      const uint8_t peer[SENSEI_RATCHET_KEY_SIZE], uint8_t dh[SENSEI_RATCHET_KEY_SIZE]) {
  if (c->x25519(c->ctx, priv, peer, dh) != 0) {
    wipe(dh, SENSEI_RATCHET_KEY_SIZE);
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  if (all_zero(dh, SENSEI_RATCHET_KEY_SIZE)) {
    return SENSEI_SECURE_ERR_DH;
  }
  return 0;
}

void sensei_ratchet_transcript(uint32_t key_id, uint32_t gen, uint32_t up_base,
                               const uint8_t e_n_pub[SENSEI_RATCHET_KEY_SIZE],
                               const uint8_t e_c_pub[SENSEI_RATCHET_KEY_SIZE], uint8_t th[SENSEI_RATCHET_TH_SIZE]) {
  uint8_t *p = th;

  memcpy(p, mix_label, MIX_LABEL_LEN);
  p += MIX_LABEL_LEN;
  put32(p, key_id);
  put32(p + 4, gen);
  put32(p + 8, up_base);
  p += 12;
  memcpy(p, e_n_pub, SENSEI_RATCHET_KEY_SIZE);
  memcpy(p + SENSEI_RATCHET_KEY_SIZE, e_c_pub, SENSEI_RATCHET_KEY_SIZE);
}

int sensei_ratchet_mix_dh(const struct sensei_secure_crypto *c, const uint8_t root[SENSEI_RATCHET_KEY_SIZE],
                          const uint8_t dh[SENSEI_RATCHET_KEY_SIZE], const uint8_t th[SENSEI_RATCHET_TH_SIZE],
                          struct sensei_ratchet_mix *out) {
  uint8_t okm[MIX_OKM];

  if (all_zero(dh, SENSEI_RATCHET_KEY_SIZE)) {
    memset(out, 0, sizeof(*out));
    return SENSEI_SECURE_ERR_DH;
  }
  if (c->hkdf(c->ctx, dh, SENSEI_RATCHET_KEY_SIZE, root, SENSEI_RATCHET_KEY_SIZE, th, SENSEI_RATCHET_TH_SIZE, okm,
              sizeof(okm)) != 0) {
    wipe(okm, sizeof(okm));
    memset(out, 0, sizeof(*out));
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  memcpy(out->root, okm, 32);
  memcpy(out->ck, okm + 32, 32);
  memcpy(out->k_down, okm + 64, 16);
  wipe(okm, sizeof(okm));
  return 0;
}

int sensei_ratchet_mix(const struct sensei_secure_crypto *c, enum sensei_secure_role role,
                       const uint8_t own_priv[SENSEI_RATCHET_KEY_SIZE], const uint8_t root[SENSEI_RATCHET_KEY_SIZE],
                       uint32_t key_id, uint32_t gen, uint32_t up_base, const uint8_t e_n_pub[SENSEI_RATCHET_KEY_SIZE],
                       const uint8_t e_c_pub[SENSEI_RATCHET_KEY_SIZE], struct sensei_ratchet_mix *out) {
  uint8_t dh[SENSEI_RATCHET_KEY_SIZE];
  uint8_t th[SENSEI_RATCHET_TH_SIZE];
  int err;

  memset(out, 0, sizeof(*out));
  err = sensei_ratchet_dh(c, own_priv, role == SENSEI_SECURE_ROLE_NODE ? e_c_pub : e_n_pub, dh);
  if (err) {
    return err;
  }
  sensei_ratchet_transcript(key_id, gen, up_base, e_n_pub, e_c_pub, th);
  err = sensei_ratchet_mix_dh(c, root, dh, th, out);
  wipe(dh, sizeof(dh));
  return err;
}

int sensei_ratchet_mk(const struct sensei_secure_crypto *c, const uint8_t ck[SENSEI_RATCHET_KEY_SIZE],
                      uint8_t mk[SENSEI_RATCHET_MK_SIZE]) {
  static const uint8_t one = 1;
  uint8_t mac[32];
  int err = c->hmac_sha256(c->ctx, ck, SENSEI_RATCHET_KEY_SIZE, &one, 1, mac);

  if (err) {
    wipe(mk, SENSEI_RATCHET_MK_SIZE);
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  memcpy(mk, mac, SENSEI_RATCHET_MK_SIZE);
  wipe(mac, sizeof(mac));
  return 0;
}

int sensei_ratchet_next(const struct sensei_secure_crypto *c, const uint8_t ck[SENSEI_RATCHET_KEY_SIZE],
                        uint8_t out[SENSEI_RATCHET_KEY_SIZE]) {
  static const uint8_t two = 2;
  uint8_t mac[32];

  if (c->hmac_sha256(c->ctx, ck, SENSEI_RATCHET_KEY_SIZE, &two, 1, mac) != 0) {
    return SENSEI_SECURE_ERR_CRYPTO; /* out (and ck, if the same) is not touched */
  }
  memcpy(out, mac, sizeof(mac));
  wipe(mac, sizeof(mac));
  return 0;
}

void sensei_ratchet_chain_init(struct sensei_ratchet_chain *ch, const uint8_t ck0[SENSEI_RATCHET_KEY_SIZE],
                               uint32_t up_base) {
  ch->up_base = up_base;
  ch->idx = 0;
  memcpy(ch->ck, ck0, SENSEI_RATCHET_KEY_SIZE);
}

void sensei_ratchet_chain_wipe(struct sensei_ratchet_chain *ch) { wipe(ch, sizeof(*ch)); }

int sensei_ratchet_chain_advance(const struct sensei_secure_crypto *c, struct sensei_ratchet_chain *ch, uint32_t idx,
                                 uint32_t max_skip) {
  uint8_t ck[SENSEI_RATCHET_KEY_SIZE];
  uint32_t steps;

  if (idx < ch->idx) {
    return SENSEI_SECURE_ERR_EPOCH;
  }
  steps = idx - ch->idx;
  if (steps > max_skip) {
    return SENSEI_SECURE_ERR_GAP;
  }
  memcpy(ck, ch->ck, sizeof(ck));
  for (uint32_t i = 0; i < steps; i++) {
    if (sensei_ratchet_next(c, ck, ck) != 0) {
      wipe(ck, sizeof(ck));
      return SENSEI_SECURE_ERR_CRYPTO;
    }
  }
  memcpy(ch->ck, ck, sizeof(ck));
  ch->idx = idx;
  wipe(ck, sizeof(ck));
  return 0;
}

int sensei_ratchet_chain_key(const struct sensei_secure_crypto *c, struct sensei_ratchet_chain *ch, uint32_t counter,
                             bool commit, uint32_t max_skip, uint8_t mk[SENSEI_RATCHET_MK_SIZE]) {
  struct sensei_ratchet_chain tmp;
  struct sensei_ratchet_chain *w = commit ? ch : &tmp;
  uint32_t ep = sensei_ratchet_epoch(counter);
  int err;

  if (ep < ch->up_base) {
    wipe(mk, SENSEI_RATCHET_MK_SIZE);
    return SENSEI_SECURE_ERR_EPOCH;
  }
  if (!commit) {
    tmp = *ch;
  }
  err = sensei_ratchet_chain_advance(c, w, ep - ch->up_base, max_skip);
  if (err == 0) {
    err = sensei_ratchet_mk(c, w->ck, mk);
  } else {
    wipe(mk, SENSEI_RATCHET_MK_SIZE);
  }
  if (!commit) {
    wipe(&tmp, sizeof(tmp));
  }
  return err;
}
