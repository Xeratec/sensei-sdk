/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_secure.h"

#include <string.h>

#define NONCE_SIZE 12u
#define HKDF_OKM 36u
#define DIR_UP 0u
#define DIR_DOWN 1u
#define MAX_HEADER SENSEI_SECURE_HEADER_NODE
#define CTR_MAX 0xFFFFFFFFu

static const uint8_t hkdf_salt[] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'E', '2', 'E', '-', 'v', '1'};

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

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static int kind_dir(unsigned kind) {
  switch (kind) {
  case SENSEI_SECURE_KIND_UPLINK:
  case SENSEI_SECURE_KIND_NODE_TO_HOST:
    return (int)DIR_UP;
  case SENSEI_SECURE_KIND_DOWNLINK:
  case SENSEI_SECURE_KIND_SETTINGS:
    return (int)DIR_DOWN;
  default:
    return -1;
  }
}

static void make_nonce(uint8_t nonce[NONCE_SIZE], unsigned dir, uint32_t counter) {
  memset(nonce, 0, NONCE_SIZE);
  nonce[0] = (uint8_t)dir;
  put32(nonce + 8, counter);
}

/* ---- keys ---- */

int sensei_secure_keygen(const struct sensei_secure_crypto *c, uint8_t priv[SENSEI_SECURE_PRIV_SIZE],
                         uint8_t pub[SENSEI_SECURE_PUB_SIZE]) {
  for (int tries = 0; tries < 16; tries++) {
    sensei_secure_key_t k = 0;

    if (c->random(c->ctx, priv, SENSEI_SECURE_PRIV_SIZE) != 0) {
      return SENSEI_SECURE_ERR_CRYPTO;
    }
    if (c->ecdh_import(c->ctx, priv, SENSEI_SECURE_PRIV_SIZE, &k) != 0) {
      continue; /* not a valid scalar (about 2^-32), try again */
    }
    int err = c->ecdh_public(c->ctx, k, pub);

    (void)c->key_destroy(c->ctx, k);
    return err ? SENSEI_SECURE_ERR_CRYPTO : 0;
  }
  wipe(priv, SENSEI_SECURE_PRIV_SIZE);
  return SENSEI_SECURE_ERR_CRYPTO;
}

int sensei_secure_derive(const struct sensei_secure_crypto *c, sensei_secure_key_t own, enum sensei_secure_role role,
                         const uint8_t node_pub[SENSEI_SECURE_PUB_SIZE],
                         const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE], struct sensei_secure_keys *out) {
  uint8_t secret[32];
  uint8_t info[2 * SENSEI_SECURE_PUB_SIZE];
  uint8_t okm[HKDF_OKM];
  int err = SENSEI_SECURE_ERR_CRYPTO;

  memcpy(info, node_pub, SENSEI_SECURE_PUB_SIZE);
  memcpy(info + SENSEI_SECURE_PUB_SIZE, backend_pub, SENSEI_SECURE_PUB_SIZE);
  if (c->ecdh_shared(c->ctx, own, role == SENSEI_SECURE_ROLE_NODE ? backend_pub : node_pub, secret) != 0) {
    goto out;
  }
  if (c->hkdf(c->ctx, secret, sizeof(secret), hkdf_salt, sizeof(hkdf_salt), info, sizeof(info), okm, sizeof(okm)) !=
      0) {
    goto out;
  }
  memcpy(out->k_up, okm, 16);
  memcpy(out->k_down, okm + 16, 16);
  out->key_id = get32(okm + 32);
  err = out->key_id == 0 ? SENSEI_SECURE_ERR_CRYPTO : 0;
out:
  wipe(secret, sizeof(secret));
  wipe(okm, sizeof(okm));
  return err;
}

/* ---- replay window ---- */

int sensei_secure_replay_check(const struct sensei_secure_replay *r, uint32_t counter) {
  uint32_t diff;

  if (counter == 0) {
    return SENSEI_SECURE_ERR_REPLAY;
  }
  if (counter > r->highest) {
    return 0;
  }
  diff = r->highest - counter;
  if (diff >= SENSEI_SECURE_WINDOW || ((r->bitmap >> diff) & 1u)) {
    return SENSEI_SECURE_ERR_REPLAY;
  }
  return 0;
}

void sensei_secure_replay_update(struct sensei_secure_replay *r, uint32_t counter) {
  if (counter > r->highest) {
    uint32_t shift = counter - r->highest;

    r->bitmap = shift < SENSEI_SECURE_WINDOW ? ((r->bitmap << shift) | 1u) : 1u;
    r->highest = counter;
  } else {
    r->bitmap |= (uint64_t)1 << (r->highest - counter);
  }
}

/* ---- session ---- */

int sensei_secure_init(struct sensei_secure *s, const struct sensei_secure_crypto *c, enum sensei_secure_role role,
                       const struct sensei_secure_keys *keys, uint32_t tx_reserved, uint32_t rx_highest,
                       uint32_t ctr_step) {
  const uint8_t *tx = role == SENSEI_SECURE_ROLE_NODE ? keys->k_up : keys->k_down;
  const uint8_t *rx = role == SENSEI_SECURE_ROLE_NODE ? keys->k_down : keys->k_up;
  void *user = s->user;
  int (*ptx)(void *, uint32_t) = s->persist_tx;
  void (*prx)(void *, uint32_t) = s->persist_rx;

  if (keys->key_id == 0 || ctr_step == 0) {
    return SENSEI_SECURE_ERR_STATE;
  }
  memset(s, 0, sizeof(*s));
  s->user = user;
  s->persist_tx = ptx;
  s->persist_rx = prx;
  s->c = c;
  s->role = role;
  if (c->aead_import(c->ctx, tx, &s->k_tx) != 0) {
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  if (c->aead_import(c->ctx, rx, &s->k_rx) != 0) {
    (void)c->key_destroy(c->ctx, s->k_tx);
    s->k_tx = 0;
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  s->key_id = keys->key_id;
  s->tx_last = tx_reserved;
  s->tx_reserved = tx_reserved;
  s->ctr_step = ctr_step;
  s->rx.highest = rx_highest;
  s->rx.bitmap = rx_highest ? ~(uint64_t)0 : 0;
  s->open = true;
  return 0;
}

void sensei_secure_close(struct sensei_secure *s) {
  if (s->open) {
    (void)s->c->key_destroy(s->c->ctx, s->k_tx);
    (void)s->c->key_destroy(s->c->ctx, s->k_rx);
  }
  s->k_tx = s->k_rx = 0;
  s->open = false;
}

struct hdr {
  unsigned dir, kind;
  bool has_node;
  uint64_t node_id;
  uint32_t key_id, counter;
  size_t size;
};

static int parse(const uint8_t *in, size_t len, struct hdr *h) {
  if (!sensei_secure_is_sealed(in, len) || ((in[0] >> 2) & 3u) != SENSEI_SECURE_VERSION) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->dir = in[0] & 1u;
  h->has_node = (in[0] & 2u) != 0;
  h->kind = in[1];
  h->size = h->has_node ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER;
  if (len < h->size + SENSEI_SECURE_TAG_SIZE || kind_dir(h->kind) != (int)h->dir) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->node_id = h->has_node ? get64(in + 2) : 0;
  h->key_id = get32(in + h->size - 8);
  h->counter = get32(in + h->size - 4);
  return 0;
}

uint32_t sensei_secure_peek_key_id(const uint8_t *buf, size_t len) {
  struct hdr h;

  return parse(buf, len, &h) == 0 ? h.key_id : 0;
}

int sensei_secure_seal(struct sensei_secure *s, enum sensei_secure_kind kind, const uint64_t *node_id,
                       const uint8_t *pt, size_t pt_len, uint8_t *out, size_t out_cap) {
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = s->role == SENSEI_SECURE_ROLE_NODE ? DIR_UP : DIR_DOWN;
  size_t hs = node_id ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER;
  size_t n = 0;
  uint32_t ctr;

  if (!s->open || s->tx_last == CTR_MAX) {
    return SENSEI_SECURE_ERR_STATE;
  }
  if (kind_dir(kind) != (int)dir) {
    return SENSEI_SECURE_ERR_KIND;
  }
  if (out_cap < hs + pt_len + SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_SPACE;
  }
  ctr = s->tx_last + 1;
  if (ctr > s->tx_reserved) {
    uint32_t res = (CTR_MAX - ctr < s->ctr_step - 1) ? CTR_MAX : ctr + s->ctr_step - 1;

    if (s->persist_tx && s->persist_tx(s->user, res) != 0) {
      return SENSEI_SECURE_ERR_STORE;
    }
    s->tx_reserved = res;
  }
  out[0] = (uint8_t)(SENSEI_SECURE_MARKER | (SENSEI_SECURE_VERSION << 2) | (node_id ? 2u : 0u) | dir);
  out[1] = (uint8_t)kind;
  if (node_id) {
    put64(out + 2, *node_id);
  }
  put32(out + hs - 8, s->key_id);
  put32(out + hs - 4, ctr);
  make_nonce(nonce, dir, ctr);
  if (s->c->aead_encrypt(s->c->ctx, s->k_tx, nonce, out, hs, pt, pt_len, out + hs, out_cap - hs, &n) != 0) {
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  s->tx_last = ctr;
  return (int)(hs + n);
}

int sensei_secure_open(struct sensei_secure *s, enum sensei_secure_kind want, const uint64_t *own_id, const uint8_t *in,
                       size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, enum sensei_secure_kind *kind) {
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = s->role == SENSEI_SECURE_ROLE_NODE ? DIR_DOWN : DIR_UP;
  struct hdr h;
  size_t n = 0;
  int err;

  if (!s->open) {
    return SENSEI_SECURE_ERR_STATE;
  }
  err = parse(in, in_len, &h);
  if (err) {
    return err;
  }
  if (h.dir != dir) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  if (want != 0 && h.kind != (unsigned)want) {
    return SENSEI_SECURE_ERR_KIND;
  }
  if (h.key_id != s->key_id) {
    return SENSEI_SECURE_ERR_KEY_ID;
  }
  if (own_id && h.has_node && h.node_id != *own_id) {
    return SENSEI_SECURE_ERR_NODE;
  }
  err = sensei_secure_replay_check(&s->rx, h.counter);
  if (err) {
    return err;
  }
  if (out_cap < in_len - h.size - SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_SPACE;
  }
  make_nonce(nonce, dir, h.counter);
  if (s->c->aead_decrypt(s->c->ctx, s->k_rx, nonce, in, h.size, in + h.size, in_len - h.size, out, out_cap, &n) != 0) {
    return SENSEI_SECURE_ERR_AUTH;
  }
  if (h.counter > s->rx.highest && s->persist_rx) {
    s->persist_rx(s->user, h.counter);
  }
  sensei_secure_replay_update(&s->rx, h.counter);
  *out_len = n;
  if (kind) {
    *kind = (enum sensei_secure_kind)h.kind;
  }
  return 0;
}
