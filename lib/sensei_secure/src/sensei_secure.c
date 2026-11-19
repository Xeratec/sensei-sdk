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

/* direction (1) | 0 0 0 | sender (4) | counter (4). The sender is 0 for the node (one sender per key). */
static void make_nonce(uint8_t nonce[NONCE_SIZE], unsigned dir, uint32_t sender, uint32_t counter) {
  memset(nonce, 0, NONCE_SIZE);
  nonce[0] = (uint8_t)dir;
  put32(nonce + 4, sender);
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

bool sensei_secure_priv_valid(const uint8_t priv[SENSEI_SECURE_PRIV_SIZE]) {
  /* order of P-256, big endian */
  static const uint8_t order[SENSEI_SECURE_PRIV_SIZE] = {
      0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
  uint8_t any = 0;
  uint32_t lt = 0, gt = 0; /* priv below / above the order, decided by the first byte that differs */

  /* No early exit and no branch on the bytes: a valid key is secret, and so is how far it is below the order. */
  for (size_t i = 0; i < SENSEI_SECURE_PRIV_SIZE; i++) {
    uint32_t a = priv[i], b = order[i];
    uint32_t undecided = (lt | gt) ^ 1u;

    any |= priv[i];
    lt |= undecided & (((a - b) >> 31) & 1u);
    gt |= undecided & (((b - a) >> 31) & 1u);
  }
  return any != 0 && lt != 0;
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
                       const struct sensei_secure_keys *keys, uint32_t tx_sender, uint32_t tx_reserved,
                       uint32_t ctr_step, const struct sensei_secure_rx_state *rx, size_t n_rx,
                       const struct sensei_secure_store *store) {
  const uint8_t *tx = role == SENSEI_SECURE_ROLE_NODE ? keys->k_up : keys->k_down;
  const uint8_t *rxk = role == SENSEI_SECURE_ROLE_NODE ? keys->k_down : keys->k_up;

  memset(s, 0, sizeof(*s));
  if (keys->key_id == 0 || ctr_step == 0 || n_rx > SENSEI_SECURE_MAX_SENDERS || (n_rx && !rx) ||
      (role == SENSEI_SECURE_ROLE_BACKEND) == (tx_sender == 0)) {
    return SENSEI_SECURE_ERR_STATE;
  }
  for (size_t i = 0; i < n_rx; i++) {
    for (size_t j = 0; j < i; j++) {
      if (rx[j].sender == rx[i].sender) {
        return SENSEI_SECURE_ERR_STATE;
      }
    }
  }
  if (c->aead_import(c->ctx, tx, &s->k_tx) != 0) {
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  if (c->aead_import(c->ctx, rxk, &s->k_rx) != 0) {
    (void)c->key_destroy(c->ctx, s->k_tx);
    s->k_tx = 0;
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  s->c = c;
  s->role = role;
  if (store) {
    s->store = *store;
  }
  s->key_id = keys->key_id;
  s->tx_sender = tx_sender;
  s->tx_last = tx_reserved;
  s->tx_reserved = tx_reserved;
  s->ctr_step = ctr_step;
  for (size_t i = 0; i < n_rx; i++) {
    s->peers[i].sender = rx[i].sender;
    s->peers[i].rx.highest = rx[i].highest;
    s->peers[i].rx.bitmap = rx[i].highest ? ~(uint64_t)0 : 0;
  }
  s->n_peers = n_rx;
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

static struct sensei_secure_peer *find_peer(struct sensei_secure *s, uint32_t sender) {
  for (size_t i = 0; i < s->n_peers; i++) {
    if (s->peers[i].sender == sender) {
      return &s->peers[i];
    }
  }
  return NULL;
}

uint32_t sensei_secure_rx_highest(const struct sensei_secure *s, uint32_t sender) {
  for (size_t i = 0; i < s->n_peers; i++) {
    if (s->peers[i].sender == sender) {
      return s->peers[i].rx.highest;
    }
  }
  return 0;
}

struct hdr {
  unsigned dir, kind;
  bool ratchet;
  unsigned gen_bits;
  bool has_node;
  uint64_t node_id;
  uint32_t key_id, sender, counter;
  size_t size;
};

static int parse(const uint8_t *in, size_t len, struct hdr *h) {
  size_t n;

  if (!sensei_secure_is_sealed(in, len)) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->dir = in[0] & 1u;
  h->has_node = (in[0] & 2u) != 0;
  if (in[1] & SENSEI_SECURE_KIND_RESERVED) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->ratchet = (in[1] & SENSEI_SECURE_KIND_RATCHET) != 0;
  if (!h->ratchet && (in[1] & SENSEI_SECURE_GEN_MASK)) { /* generation bits without the ratchet bit */
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->gen_bits = h->ratchet ? (unsigned)((in[1] & SENSEI_SECURE_GEN_MASK) >> SENSEI_SECURE_GEN_SHIFT) : 0u;
  h->kind = in[1] & SENSEI_SECURE_KIND_MASK;
  n = (h->has_node ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER) +
      (h->dir == DIR_DOWN ? SENSEI_SECURE_SENDER_SIZE : 0u);
  h->size = n;
  if (len < n + SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  if (h->kind == SENSEI_SECURE_KIND_COMMISSION) { /* both directions, but nothing else that the other kinds have */
    if (h->ratchet || h->has_node) {
      return SENSEI_SECURE_ERR_FORMAT;
    }
  } else if (kind_dir(h->kind) != (int)h->dir) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  h->node_id = h->has_node ? get64(in + 2) : 0;
  h->key_id = get32(in + (h->has_node ? 10 : 2));
  h->sender = h->dir == DIR_DOWN ? get32(in + n - 8) : 0;
  h->counter = get32(in + n - 4);
  if (h->kind == SENSEI_SECURE_KIND_COMMISSION && h->dir == DIR_DOWN && h->sender != SENSEI_SECURE_COMMISSION_SENDER) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  return h->dir == DIR_DOWN && h->sender == 0 ? SENSEI_SECURE_ERR_FORMAT : 0;
}

uint32_t sensei_secure_peek_key_id(const uint8_t *buf, size_t len) {
  struct hdr h;

  return parse(buf, len, &h) == 0 ? h.key_id : 0;
}

int sensei_secure_peek(const uint8_t *buf, size_t len, struct sensei_secure_info *info) {
  struct hdr h;
  int err = parse(buf, len, &h);

  if (err) {
    return err;
  }
  info->kind = h.kind;
  info->to_node = h.dir == DIR_DOWN;
  info->ratchet = h.ratchet;
  info->gen_bits = h.gen_bits;
  info->has_node_id = h.has_node;
  info->node_id = h.node_id;
  info->key_id = h.key_id;
  info->sender = h.sender;
  info->counter = h.counter;
  return 0;
}

void sensei_secure_set_keyring(struct sensei_secure *s, const struct sensei_secure_keyring *ring) {
  if (ring) {
    s->ring = *ring;
  } else {
    memset(&s->ring, 0, sizeof(s->ring));
  }
}

int sensei_secure_tx_jump(struct sensei_secure *s, uint32_t next) {
  if (!s->open || next == 0) {
    return SENSEI_SECURE_ERR_STATE;
  }
  if (next - 1 > s->tx_last) {
    s->tx_last = next - 1;
  }
  return 0;
}

int sensei_secure_seal(struct sensei_secure *s, enum sensei_secure_kind kind, const uint64_t *node_id,
                       const uint8_t *pt, size_t pt_len, uint8_t *out, size_t out_cap) {
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = s->role == SENSEI_SECURE_ROLE_NODE ? DIR_UP : DIR_DOWN;
  size_t hs =
      (node_id ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER) + (dir == DIR_DOWN ? SENSEI_SECURE_SENDER_SIZE : 0u);
  size_t n = 0;
  uint32_t ctr, gen = 0;
  sensei_secure_key_t key = s->k_tx, tmp = 0;
  int err = 0;

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

    if (s->store.tx_block) {
      if (s->store.tx_block(s->store.user, ctr, &res) != 0 || res < ctr) {
        return SENSEI_SECURE_ERR_STORE;
      }
    } else if (s->store.tx && s->store.tx(s->store.user, res) != 0) {
      return SENSEI_SECURE_ERR_STORE;
    }
    s->tx_reserved = res;
  }
  if (s->ring.tx_key) {
    uint8_t gkey[SENSEI_SECURE_KEY_SIZE] = {0}; /* a hook that returns 0 without writing leaves zeros, not stack */

    if (s->ring.tx_key(s->ring.user, ctr, &gen, gkey) != 0) {
      wipe(gkey, sizeof(gkey));
      return SENSEI_SECURE_ERR_TXKEY;
    }
    if (gen != 0) {
      if (s->c->aead_import(s->c->ctx, gkey, &tmp) != 0) {
        wipe(gkey, sizeof(gkey));
        return SENSEI_SECURE_ERR_CRYPTO;
      }
      key = tmp;
    }
    wipe(gkey, sizeof(gkey));
  }
  out[0] = (uint8_t)(SENSEI_SECURE_MARKER | (SENSEI_SECURE_VERSION << 2) | (node_id ? 2u : 0u) | dir);
  out[1] =
      (uint8_t)(gen == 0 ? (unsigned)kind
                         : (SENSEI_SECURE_KIND_RATCHET | ((gen & 3u) << SENSEI_SECURE_GEN_SHIFT) | (unsigned)kind));
  if (node_id) {
    put64(out + 2, *node_id);
  }
  put32(out + (node_id ? 10 : 2), s->key_id);
  if (dir == DIR_DOWN) {
    put32(out + hs - 8, s->tx_sender);
  }
  put32(out + hs - 4, ctr);
  make_nonce(nonce, dir, s->tx_sender, ctr);
  err = s->c->aead_encrypt(s->c->ctx, key, nonce, out, hs, pt, pt_len, out + hs, out_cap - hs, &n);
  if (tmp) {
    (void)s->c->key_destroy(s->c->ctx, tmp);
  }
  if (err != 0) {
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  s->tx_last = ctr;
  return (int)(hs + n);
}

/* The refusals a keyring hook may report. Anything else (an errno of a storage or crypto call) is a missing key: the
 * codes of this library and the errnos share small negative numbers, so a pass-through would name the wrong error. */
static int rx_hook_code(int err) {
  switch (err) {
  case SENSEI_SECURE_ERR_GEN:
  case SENSEI_SECURE_ERR_EPOCH:
  case SENSEI_SECURE_ERR_GAP:
  case SENSEI_SECURE_ERR_DH:
  case SENSEI_SECURE_ERR_CRYPTO:
    return err;
  default:
    return SENSEI_SECURE_ERR_GEN;
  }
}

int sensei_secure_open(struct sensei_secure *s, enum sensei_secure_kind want, const uint64_t *own_id, const uint8_t *in,
                       size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, enum sensei_secure_kind *kind) {
  static const struct sensei_secure_replay none = {0, 0};
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = s->role == SENSEI_SECURE_ROLE_NODE ? DIR_DOWN : DIR_UP;
  struct sensei_secure_peer *peer;
  struct hdr h;
  sensei_secure_key_t key = s->k_rx, tmp = 0;
  size_t n = 0;
  int err;

  if (!s->open) {
    return SENSEI_SECURE_ERR_STATE;
  }
  err = parse(in, in_len, &h);
  if (err) {
    return err;
  }
  if (h.dir != dir || h.kind == SENSEI_SECURE_KIND_COMMISSION || want == SENSEI_SECURE_KIND_COMMISSION) {
    /* Kind 5 belongs to the commissioning channel (sensei_secure_open_commission()): refused here whatever the
     * keys, also when any kind is allowed (want 0). It consumes no counter and no window. */
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
  peer = find_peer(s, h.sender);
  if (!peer && s->n_peers == SENSEI_SECURE_MAX_SENDERS) {
    return SENSEI_SECURE_ERR_SENDER;
  }
  err = sensei_secure_replay_check(peer ? &peer->rx : &none, h.counter);
  if (err) {
    return err;
  }
  if (out_cap < in_len - h.size - SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_SPACE;
  }
  if (h.ratchet || s->ring.rx_key) {
    uint8_t gkey[SENSEI_SECURE_KEY_SIZE] = {0};

    err = h.ratchet && !s->ring.rx_key ? SENSEI_SECURE_ERR_GEN
                                       : s->ring.rx_key(s->ring.user, h.ratchet, h.gen_bits, h.counter, gkey);
    if (err < 0) {
      wipe(gkey, sizeof(gkey));
      return rx_hook_code(err);
    }
    if (err > 0 && h.ratchet) { /* "use the session key" is for generation 0 only */
      wipe(gkey, sizeof(gkey));
      return SENSEI_SECURE_ERR_GEN;
    }
    if (err == 0) {
      if (s->c->aead_import(s->c->ctx, gkey, &tmp) != 0) {
        wipe(gkey, sizeof(gkey));
        return SENSEI_SECURE_ERR_CRYPTO;
      }
      key = tmp;
    }
    wipe(gkey, sizeof(gkey));
  }
  make_nonce(nonce, dir, h.sender, h.counter);
  err = s->c->aead_decrypt(s->c->ctx, key, nonce, in, h.size, in + h.size, in_len - h.size, out, out_cap, &n);
  if (tmp) {
    (void)s->c->key_destroy(s->c->ctx, tmp);
  }
  if (err != 0) {
    return SENSEI_SECURE_ERR_AUTH;
  }
  if (h.counter > (peer ? peer->rx.highest : 0u) && s->store.rx &&
      s->store.rx(s->store.user, h.sender, h.counter) != 0) {
    wipe(out, n); /* refused: do not hand out the plaintext */
    return SENSEI_SECURE_ERR_STORE;
  }
  if (!peer) {
    peer = &s->peers[s->n_peers++];
    peer->sender = h.sender;
    peer->rx.highest = 0;
    peer->rx.bitmap = 0;
  }
  sensei_secure_replay_update(&peer->rx, h.counter);
  *out_len = n;
  if (kind) {
    *kind = (enum sensei_secure_kind)h.kind;
  }
  return 0;
}

/* ---- kind 5: the commissioning channel ---- */

int sensei_secure_seal_commission(const struct sensei_secure_crypto *c, sensei_secure_key_t key, uint32_t session_id,
                                  bool to_node, uint32_t counter, const uint8_t *pt, size_t pt_len, uint8_t *out,
                                  size_t out_cap) {
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = to_node ? DIR_DOWN : DIR_UP;
  size_t hs = SENSEI_SECURE_HEADER + (to_node ? SENSEI_SECURE_SENDER_SIZE : 0u);
  size_t n = 0;
  uint32_t sender = to_node ? SENSEI_SECURE_COMMISSION_SENDER : 0u;

  if (counter == 0) {
    return SENSEI_SECURE_ERR_STATE;
  }
  if (pt_len > SENSEI_SECURE_COMMISSION_MAX || hs + pt_len + SENSEI_SECURE_TAG_SIZE > SENSEI_SECURE_COMMISSION_MAX ||
      out_cap < hs + pt_len + SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_SPACE;
  }
  out[0] = (uint8_t)(SENSEI_SECURE_MARKER | (SENSEI_SECURE_VERSION << 2) | dir);
  out[1] = (uint8_t)SENSEI_SECURE_KIND_COMMISSION;
  put32(out + 2, session_id);
  if (to_node) {
    put32(out + 6, sender);
  }
  put32(out + hs - 4, counter);
  make_nonce(nonce, dir, sender, counter);
  if (c->aead_encrypt(c->ctx, key, nonce, out, hs, pt, pt_len, out + hs, out_cap - hs, &n) != 0) {
    return SENSEI_SECURE_ERR_CRYPTO;
  }
  return (int)(hs + n);
}

int sensei_secure_open_commission(const struct sensei_secure_crypto *c, sensei_secure_key_t key, uint32_t session_id,
                                  bool from_node, struct sensei_secure_replay *rx, const uint8_t *in, size_t in_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len) {
  uint8_t nonce[NONCE_SIZE];
  unsigned dir = from_node ? DIR_UP : DIR_DOWN;
  struct hdr h;
  size_t n = 0;
  int err;

  if (in_len > SENSEI_SECURE_COMMISSION_MAX) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  err = parse(in, in_len, &h);
  if (err) {
    return err;
  }
  if (h.kind != SENSEI_SECURE_KIND_COMMISSION || h.dir != dir || h.key_id != session_id) {
    return SENSEI_SECURE_ERR_FORMAT;
  }
  err = sensei_secure_replay_check(rx, h.counter);
  if (err) {
    return err;
  }
  if (out_cap < in_len - h.size - SENSEI_SECURE_TAG_SIZE) {
    return SENSEI_SECURE_ERR_SPACE;
  }
  make_nonce(nonce, dir, h.sender, h.counter);
  if (c->aead_decrypt(c->ctx, key, nonce, in, h.size, in + h.size, in_len - h.size, out, out_cap, &n) != 0) {
    return SENSEI_SECURE_ERR_AUTH;
  }
  sensei_secure_replay_update(rx, h.counter);
  *out_len = n;
  return 0;
}
