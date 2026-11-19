/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Forward secrecy primitives (sensei_ratchet.c) and the generation 1 and later envelopes, against the vectors of the
 * Python implementation (R_0, X25519 of RFC 7748, the mix, the chain, envelopes, a scripted transcript), and a property
 * test: no (key, nonce) pair twice over 1e5 messages and 20 mixes. */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "sensei_ratchet.h"
#include "sensei_secure.h"
#include "vectors.h"

static const struct sensei_secure_crypto *C;

#define BSENDER 0x01020304u

static struct sensei_secure_keys fixed_keys(void) {
  struct sensei_secure_keys k;
  sensei_secure_key_t own = 0;

  CHECK(C->ecdh_import(C->ctx, vec_sec_node_priv, 32, &own) == 0);
  CHECK(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_NODE, vec_sec_node_pub, vec_sec_backend_pub, &k) == 0);
  C->key_destroy(C->ctx, own);
  return k;
}

static void open_session(struct sensei_secure *s, enum sensei_secure_role role, uint32_t sender, uint32_t tx_res) {
  struct sensei_secure_keys k = fixed_keys();

  memset(s, 0xA5, sizeof(*s));
  CHECK(sensei_secure_init(s, C, role, &k, role == SENSEI_SECURE_ROLE_NODE ? 0 : sender, tx_res, 1, NULL, 0, NULL) ==
        0);
}

/* ---- 1. R_0, 2. X25519 ---- */

static void test_root0(void) {
  struct sensei_secure_keys k = fixed_keys();
  uint8_t root[32];

  CHECK(sensei_ratchet_available(C));
  CHECK(sensei_ratchet_root0(C, &k, root) == 0 && memcmp(root, vec_rat_root0, 32) == 0);
  k.key_id ^= 1; /* the key id is part of the derivation */
  CHECK(sensei_ratchet_root0(C, &k, root) == 0 && memcmp(root, vec_rat_root0, 32) != 0);
}

static void test_x25519(void) {
  uint8_t out[32], priv[32], pub[32], pub2[32], a[32], b[32];

  for (int i = 0; i < VEC_RAT_X25519_COUNT; i++) {
    memset(out, 0xEE, sizeof(out));
    CHECK(C->x25519(C->ctx, vec_rat_x25519[i].priv, vec_rat_x25519[i].peer, out) == 0);
    CHECK(memcmp(out, vec_rat_x25519[i].out, 32) == 0);
    CHECK(sensei_ratchet_dh(C, vec_rat_x25519[i].priv, vec_rat_x25519[i].peer, out) == 0);
    CHECK(memcmp(out, vec_rat_x25519[i].out, 32) == 0);
  }
  /* public key = X25519(priv, 9): RFC 7748 section 6.1 */
  CHECK(sensei_ratchet_public(C, vec_rat_x25519[2].priv, pub) == 0 && memcmp(pub, vec_rat_x25519[2].out, 32) == 0);
  /* a point of low order: the backend fails or gives zeros, the ratchet refuses either way and wipes dh */
  for (int i = 0; i < VEC_RAT_LOW_COUNT; i++) {
    int rc;

    memset(out, 0xEE, sizeof(out));
    rc = sensei_ratchet_dh(C, vec_rat_x25519[4].priv, vec_rat_low[i], out);
    CHECK(rc == SENSEI_SECURE_ERR_DH || rc == SENSEI_SECURE_ERR_CRYPTO);
    CHECK(rc == SENSEI_SECURE_ERR_DH || out[0] == 0); /* wiped on a backend failure */
  }
  /* the top bit of u is ignored (RFC 7748): the same result with it set */
  memcpy(pub, vec_rat_x25519[1].peer, 32);
  pub[31] ^= 0x80;
  CHECK(C->x25519(C->ctx, vec_rat_x25519[1].priv, pub, out) == 0 && memcmp(out, vec_rat_x25519[1].out, 32) == 0);
  /* fresh keys agree */
  CHECK(C->x25519_keygen(C->ctx, a, pub) == 0 && C->x25519_keygen(C->ctx, b, pub2) == 0);
  CHECK(memcmp(a, b, 32) != 0 && memcmp(pub, pub2, 32) != 0);
  CHECK(sensei_ratchet_public(C, a, out) == 0 && memcmp(out, pub, 32) == 0);
  CHECK(sensei_ratchet_dh(C, a, pub2, priv) == 0 && sensei_ratchet_dh(C, b, pub, out) == 0 &&
        memcmp(priv, out, 32) == 0);
}

/* ---- 3. mix ---- */

static void test_mix(void) {
  for (int i = 0; i < VEC_RAT_MIX_COUNT; i++) {
    const vec_rat_mix_t *v = &vec_rat_mix[i];
    struct sensei_ratchet_mix n, c, d;
    uint8_t th[SENSEI_RATCHET_TH_SIZE], dh[32], pub[32];

    CHECK(sensei_ratchet_public(C, v->e_n_priv, pub) == 0 && memcmp(pub, v->e_n_pub, 32) == 0);
    CHECK(sensei_ratchet_public(C, v->e_c_priv, pub) == 0 && memcmp(pub, v->e_c_pub, 32) == 0);
    sensei_ratchet_transcript(VEC_SEC_KEY_ID, v->gen, v->up_base, v->e_n_pub, v->e_c_pub, th);
    CHECK(memcmp(th, v->th, sizeof(th)) == 0);
    CHECK(sensei_ratchet_dh(C, v->e_n_priv, v->e_c_pub, dh) == 0 && memcmp(dh, v->dh, 32) == 0);
    CHECK(sensei_ratchet_mix_dh(C, v->root, dh, th, &d) == 0);
    CHECK(memcmp(d.root, v->root_out, 32) == 0 && memcmp(d.ck, v->ck, 32) == 0 && memcmp(d.k_down, v->k_down, 16) == 0);
    /* both sides of the exchange */
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->root, VEC_SEC_KEY_ID, v->gen, v->up_base,
                             v->e_n_pub, v->e_c_pub, &n) == 0);
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_BACKEND, v->e_c_priv, v->root, VEC_SEC_KEY_ID, v->gen, v->up_base,
                             v->e_n_pub, v->e_c_pub, &c) == 0);
    CHECK(memcmp(&n, &c, sizeof(n)) == 0 && memcmp(&n, &d, sizeof(n)) == 0);
    /* every input is bound: root, key id, generation, up_base, both public keys */
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->root, VEC_SEC_KEY_ID ^ 1u, v->gen, v->up_base,
                             v->e_n_pub, v->e_c_pub, &c) == 0 &&
          memcmp(&n, &c, sizeof(n)) != 0);
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->root, VEC_SEC_KEY_ID, v->gen + 1, v->up_base,
                             v->e_n_pub, v->e_c_pub, &c) == 0 &&
          memcmp(&n, &c, sizeof(n)) != 0);
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->root, VEC_SEC_KEY_ID, v->gen, v->up_base + 1,
                             v->e_n_pub, v->e_c_pub, &c) == 0 &&
          memcmp(&n, &c, sizeof(n)) != 0);
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->e_c_pub /* other root */, VEC_SEC_KEY_ID,
                             v->gen, v->up_base, v->e_n_pub, v->e_c_pub, &c) == 0 &&
          memcmp(&n, &c, sizeof(n)) != 0);
    /* a low-order peer: no keys, the output is zero */
    for (int j = 0; j < VEC_RAT_LOW_COUNT; j++) {
      int rc = sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, v->root, VEC_SEC_KEY_ID, v->gen, v->up_base,
                                  v->e_n_pub, vec_rat_low[j], &c);

      CHECK(rc == SENSEI_SECURE_ERR_DH || rc == SENSEI_SECURE_ERR_CRYPTO);
      CHECK(c.root[0] == 0 && c.ck[0] == 0 && c.k_down[0] == 0);
    }
  }
  /* an all-zero dh is refused by the KDF step itself */
  {
    uint8_t zero[32] = {0}, th[SENSEI_RATCHET_TH_SIZE] = {0};
    struct sensei_ratchet_mix m;

    CHECK(sensei_ratchet_mix_dh(C, vec_rat_mix[0].root, zero, th, &m) == SENSEI_SECURE_ERR_DH);
  }
}

/* ---- 4. chain ---- */

static void test_chain(void) {
  struct sensei_ratchet_chain ch;
  uint8_t mk[16], ck[32];
  const uint32_t base = VEC_RAT_UP_BASE_1;

  sensei_ratchet_chain_init(&ch, vec_rat_mix[0].ck, base);
  CHECK(ch.idx == 0 && ch.up_base == base);
  for (uint32_t i = 0; i < 4; i++) {
    CHECK(sensei_ratchet_chain_advance(C, &ch, i, SENSEI_RATCHET_MAX_SKIP) == 0);
    CHECK(ch.idx == i && memcmp(ch.ck, vec_rat_chain_ck + 32 * i, 32) == 0);
    CHECK(sensei_ratchet_mk(C, ch.ck, mk) == 0 && memcmp(mk, vec_rat_chain_mk + 16 * i, 16) == 0);
  }
  CHECK(sensei_ratchet_chain_advance(C, &ch, 1000, SENSEI_RATCHET_MAX_SKIP) == 0 && ch.idx == 1000);
  CHECK(memcmp(ch.ck, vec_rat_chain_ck1000, 32) == 0);
  /* the in place step */
  memcpy(ck, vec_rat_chain_ck, 32);
  CHECK(sensei_ratchet_next(C, ck, ck) == 0 && memcmp(ck, vec_rat_chain_ck + 32, 32) == 0);

  /* message key by counter: epoch(counter) - up_base, without and with commit */
  sensei_ratchet_chain_init(&ch, vec_rat_mix[0].ck, base);
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t c = 64 * (base + i) + (i * 21) % 64;

    CHECK(sensei_ratchet_epoch(c) == base + i);
    CHECK(sensei_ratchet_chain_key(C, &ch, c, false, SENSEI_RATCHET_MAX_SKIP, mk) == 0);
    CHECK(memcmp(mk, vec_rat_chain_mk + 16 * i, 16) == 0 && ch.idx == 0); /* not committed */
  }
  CHECK(sensei_ratchet_chain_key(C, &ch, 64 * (base + 3), true, SENSEI_RATCHET_MAX_SKIP, mk) == 0);
  CHECK(ch.idx == 3 && memcmp(mk, vec_rat_chain_mk + 48, 16) == 0 && memcmp(ch.ck, vec_rat_chain_ck + 96, 32) == 0);
  /* the same epoch again: the 64 counters of an epoch share a key */
  CHECK(sensei_ratchet_chain_key(C, &ch, 64 * (base + 3) + 63, true, SENSEI_RATCHET_MAX_SKIP, mk) == 0 && ch.idx == 3);
  CHECK(memcmp(mk, vec_rat_chain_mk + 48, 16) == 0);
  /* behind the chain, or before its start: refused, nothing changes, the key is wiped */
  memset(mk, 0xEE, sizeof(mk));
  CHECK(sensei_ratchet_chain_key(C, &ch, 64 * (base + 2) + 5, true, SENSEI_RATCHET_MAX_SKIP, mk) ==
        SENSEI_SECURE_ERR_EPOCH);
  CHECK(mk[0] == 0 && ch.idx == 3);
  memset(mk, 0xEE, sizeof(mk));
  CHECK(sensei_ratchet_chain_key(C, &ch, 64 * (base - 1) + 63, false, SENSEI_RATCHET_MAX_SKIP, mk) ==
        SENSEI_SECURE_ERR_EPOCH);
  CHECK(mk[0] == 0 && ch.idx == 3);
  CHECK(sensei_ratchet_chain_advance(C, &ch, 2, SENSEI_RATCHET_MAX_SKIP) == SENSEI_SECURE_ERR_EPOCH && ch.idx == 3);
  /* MAX_SKIP: at the limit it walks, one more is refused and the chain stays */
  sensei_ratchet_chain_init(&ch, vec_rat_mix[0].ck, base);
  CHECK(sensei_ratchet_chain_advance(C, &ch, 100, 100) == 0 && ch.idx == 100);
  CHECK(sensei_ratchet_chain_advance(C, &ch, 201, 100) == SENSEI_SECURE_ERR_GAP && ch.idx == 100);
  CHECK(sensei_ratchet_chain_advance(C, &ch, 200, 100) == 0 && ch.idx == 200);
  CHECK(SENSEI_RATCHET_MAX_SKIP == (1u << 20));
  sensei_ratchet_chain_init(&ch, vec_rat_mix[0].ck, base);
  CHECK(sensei_ratchet_chain_key(C, &ch, 64 * (base + SENSEI_RATCHET_MAX_SKIP + 1), false, SENSEI_RATCHET_MAX_SKIP,
                                 mk) == SENSEI_SECURE_ERR_GAP);
  CHECK(sensei_ratchet_chain_key(C, &ch, 0xFFFFFFFFu, true, SENSEI_RATCHET_MAX_SKIP, mk) == SENSEI_SECURE_ERR_GAP);
  CHECK(ch.idx == 0 && memcmp(ch.ck, vec_rat_mix[0].ck, 32) == 0);
  sensei_ratchet_chain_wipe(&ch);
  CHECK(ch.up_base == 0 && ch.ck[0] == 0);
}

static void test_epoch_math(void) {
  CHECK(sensei_ratchet_epoch(0) == 0 && sensei_ratchet_epoch(63) == 0 && sensei_ratchet_epoch(64) == 1);
  CHECK(sensei_ratchet_epoch(127) == 1 && sensei_ratchet_epoch(128) == 2 &&
        sensei_ratchet_epoch(0xFFFFFFFFu) == 0x3FFFFFFu);
  CHECK(SENSEI_RATCHET_EPOCH_SIZE == SENSEI_SECURE_WINDOW); /* one epoch = one replay window */
}

/* ---- 5. envelopes of generation 1 and later ---- */

/* The keyring of a test session: generation 1 and generation 4 (bits 0 with the ratchet bit) of vec_rat_mix. */
#define FX_SKIP 5000u /* a small limit: a flipped counter bit must not make the test walk a million steps */

struct fx {
  bool node;                            /* a node session: seals uplinks (chain), opens downlinks (k_down) */
  uint32_t seal_gen;                    /* generation of the next seal: 1 or 4 */
  struct sensei_ratchet_chain chain[2]; /* generation 1, generation 4 */
  const uint8_t *k_down[2];
  int tx_calls, rx_calls;
};

static void fx_init(struct fx *f, bool node) {
  memset(f, 0, sizeof(*f));
  f->node = node;
  sensei_ratchet_chain_init(&f->chain[0], vec_rat_mix[0].ck, vec_rat_mix[0].up_base);
  sensei_ratchet_chain_init(&f->chain[1], vec_rat_mix[1].ck, vec_rat_mix[1].up_base);
  f->k_down[0] = vec_rat_mix[0].k_down;
  f->k_down[1] = vec_rat_mix[1].k_down;
}

static int fx_key(struct fx *f, bool up, unsigned which, uint32_t counter, uint8_t key[16]) {
  if (up) {
    return sensei_ratchet_chain_key(C, &f->chain[which], counter, false, FX_SKIP, key);
  }
  memcpy(key, f->k_down[which], 16);
  return 0;
}

static int fx_tx(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  struct fx *f = u;

  f->tx_calls++;
  *gen = f->seal_gen;
  return fx_key(f, f->node, f->seal_gen == 1 ? 0 : 1, counter, key);
}

static int fx_rx(void *u, bool ratchet, unsigned bits, uint32_t counter, uint8_t key[16]) {
  struct fx *f = u;

  f->rx_calls++;
  if (!ratchet) {
    return 1; /* the session key */
  }
  if (bits != 1 && bits != 0) {
    return SENSEI_SECURE_ERR_GEN;
  }
  return fx_key(f, !f->node, bits == 1 ? 0 : 1, counter, key);
}

static void set_ring(struct sensei_secure *s, struct fx *f) {
  struct sensei_secure_keyring ring = {.user = f, .tx_key = fx_tx, .rx_key = fx_rx};

  sensei_secure_set_keyring(s, &ring);
}

static bool is_up(unsigned kind) {
  return kind == SENSEI_SECURE_KIND_UPLINK || kind == SENSEI_SECURE_KIND_NODE_TO_HOST;
}

static void test_envelopes(void) {
  for (int i = 0; i < VEC_RAT_ENV_COUNT; i++) {
    const vec_rat_env_t *v = &vec_rat_env[i];
    const bool up = is_up(v->kind);
    struct sensei_secure tx, rx;
    struct fx ftx, frx;
    uint8_t pt[600];
    size_t n = 0;
    enum sensei_secure_kind kind = 0;
    struct sensei_secure_info info;
    int m;

    fx_init(&ftx, up);
    fx_init(&frx, !up);
    ftx.seal_gen = v->gen;
    open_session(&rx, up ? SENSEI_SECURE_ROLE_BACKEND : SENSEI_SECURE_ROLE_NODE, up ? BSENDER : 0, 0);
    set_ring(&rx, &frx);
    if (v->expect == 0) {
      uint8_t out[600];

      /* C seals the same bytes as Python, and the other side opens them */
      open_session(&tx, up ? SENSEI_SECURE_ROLE_NODE : SENSEI_SECURE_ROLE_BACKEND, v->sender, v->counter - 1);
      set_ring(&tx, &ftx);
      m = sensei_secure_seal(&tx, (enum sensei_secure_kind)v->kind, v->has_node_id ? &v->node_id : NULL, v->plain,
                             v->plain_len, out, sizeof(out));
      CHECK(m == (int)v->sealed_len && ftx.tx_calls == 1);
      CHECK(m > 0 && memcmp(out, v->sealed, v->sealed_len) == 0);
      CHECK(tx.tx_last == v->counter);
      sensei_secure_close(&tx);
    }
    /* the header: kind without the generation bits, the generation bits, the ratchet flag */
    CHECK(sensei_secure_is_sealed(v->sealed, v->sealed_len));
    if (v->expect == 0 || v->expect == SENSEI_SECURE_ERR_AUTH || v->expect == SENSEI_SECURE_ERR_GEN ||
        v->expect == SENSEI_SECURE_ERR_EPOCH) {
      CHECK(sensei_secure_peek(v->sealed, v->sealed_len, &info) == 0);
      CHECK(info.counter == v->counter && info.key_id == VEC_SEC_KEY_ID && info.ratchet && info.to_node == !up);
    }
    if (v->expect == 0) {
      CHECK(sensei_secure_kind(v->sealed) == v->kind && sensei_secure_is_ratchet(v->sealed));
      CHECK(sensei_secure_gen_bits(v->sealed) == (v->gen & 3u) && info.gen_bits == (v->gen & 3u));
      CHECK(v->sealed[1] == (0x80u | ((v->gen & 3u) << 4) | v->kind));
      CHECK(info.kind == v->kind && info.sender == v->sender);
      CHECK(sensei_secure_peek_key_id(v->sealed, v->sealed_len) == VEC_SEC_KEY_ID);
    }
    m = sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, &kind);
    CHECK(m == v->expect);
    if (v->expect == 0) {
      CHECK(n == v->plain_len && (n == 0 || memcmp(pt, v->plain, n) == 0) && kind == v->kind);
      CHECK(sensei_secure_rx_highest(&rx, v->sender) == v->counter);
      /* the second time: a replay, and the hook is not asked */
      frx.rx_calls = 0;
      CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, &kind) ==
            SENSEI_SECURE_ERR_REPLAY);
      CHECK(frx.rx_calls == 0);
    } else {
      CHECK(rx.n_peers == 0); /* nothing recorded */
    }
    sensei_secure_close(&rx);
  }
}

/* Every bit of the header and the ciphertext is covered, the generation bits included. */
static void test_envelope_tamper(void) {
  const vec_rat_env_t *v = &vec_rat_env[1]; /* uplink, generation 1, idx 5 */
  struct sensei_secure rx;
  struct fx f;
  uint8_t bad[300], pt[300];
  size_t n;

  fx_init(&f, false);
  open_session(&rx, SENSEI_SECURE_ROLE_BACKEND, BSENDER, 0);
  set_ring(&rx, &f);
  for (size_t b = 0; b < v->sealed_len; b++) {
    memcpy(bad, v->sealed, v->sealed_len);
    bad[b] ^= 0x01;
    CHECK(sensei_secure_open(&rx, 0, NULL, bad, v->sealed_len, pt, sizeof(pt), &n, NULL) < 0);
  }
  CHECK(rx.n_peers == 0);
  CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, NULL) == 0);
  sensei_secure_close(&rx);
}

/* ---- keyring hooks and the session ---- */

static int hook_rx_code;
static int hook_rx_calls;
static int hook_rx(void *u, bool ratchet, unsigned bits, uint32_t counter, uint8_t key[16]) {
  (void)u;
  (void)bits;
  (void)counter;
  (void)ratchet;
  hook_rx_calls++;
  memset(key, 0x55, 16);
  return hook_rx_code;
}

static int hook_tx_fail(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  (void)u;
  (void)counter;
  (void)gen;
  (void)key;
  return -1;
}

static void test_keyring(void) {
  const vec_rat_env_t *up = &vec_rat_env[0], *down = &vec_rat_env[3];
  struct sensei_secure back, node;
  struct sensei_secure_keyring ring = {.rx_key = hook_rx};
  uint8_t pt[300], env[300];
  size_t n;
  int m;

  /* no keyring: a ratchet envelope has no key (the same for an uplink and for a downlink) */
  open_session(&back, SENSEI_SECURE_ROLE_BACKEND, BSENDER, 0);
  open_session(&node, SENSEI_SECURE_ROLE_NODE, 0, 0);
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_GEN);
  CHECK(sensei_secure_open(&node, 0, NULL, down->sealed, down->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_GEN);
  CHECK(back.n_peers == 0 && node.n_peers == 0);

  /* hook results: negative codes pass (known ones), unknown ones become ERR_GEN, positive is for generation 0 only */
  sensei_secure_set_keyring(&back, &ring);
  hook_rx_calls = 0;
  hook_rx_code = SENSEI_SECURE_ERR_EPOCH;
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_EPOCH);
  hook_rx_code = SENSEI_SECURE_ERR_GAP;
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_GAP);
  hook_rx_code = -100;
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_GEN);
  hook_rx_code = 1;
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_GEN);
  hook_rx_code = 0; /* a key that is not the right one: the tag fails */
  CHECK(sensei_secure_open(&back, 0, NULL, up->sealed, up->sealed_len, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_AUTH);
  CHECK(hook_rx_calls == 5 && back.n_peers == 0);

  /* the hook decides about generation 0 too: refuse it (a node of generation 1 or later) or use the session key */
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 5, env, sizeof(env));
  CHECK(m == 5 + 26 && env[1] == SENSEI_SECURE_KIND_UPLINK); /* generation 0 is unchanged: the plain kind byte */
  CHECK(sensei_secure_kind(env) == SENSEI_SECURE_KIND_UPLINK && !sensei_secure_is_ratchet(env) &&
        sensei_secure_gen_bits(env) == 0);
  hook_rx_code = SENSEI_SECURE_ERR_GEN;
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_GEN);
  hook_rx_code = 1;
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0 && n == 5);
  sensei_secure_set_keyring(&back, NULL); /* removed: generation 0 as before */
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 5, env, sizeof(env));
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);

  /* a seal hook that fails: ERR_GEN, the counter is not used, nothing is sealed */
  ring = (struct sensei_secure_keyring){.tx_key = hook_tx_fail};
  sensei_secure_set_keyring(&node, &ring);
  CHECK(node.tx_last == 2);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 5, env, sizeof(env)) == SENSEI_SECURE_ERR_GEN);
  CHECK(node.tx_last == 2);
  sensei_secure_set_keyring(&node, NULL);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 5, env, sizeof(env)) == 5 + 26 &&
        node.tx_last == 3);

  /* tx_jump: forward only, the next seal carries the counter, 0 is refused */
  CHECK(sensei_secure_tx_jump(&node, 64) == 0 && node.tx_last == 63);
  CHECK(sensei_secure_tx_jump(&node, 10) == 0 && node.tx_last == 63); /* never lowers */
  CHECK(sensei_secure_tx_jump(&node, 0) == SENSEI_SECURE_ERR_STATE);
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 5, env, sizeof(env));
  CHECK(m > 0 && sensei_secure_peek(env, (size_t)m, &(struct sensei_secure_info){0}) == 0 && node.tx_last == 64);
  sensei_secure_close(&node);
  CHECK(sensei_secure_tx_jump(&node, 100) == SENSEI_SECURE_ERR_STATE);
  sensei_secure_close(&back);
}

/* The jump is kept by the persisted mark: after it the next seal stores a new high-water mark above the counter. */
static int store_calls;
static uint32_t store_mark;
static int store_tx(void *u, uint32_t reserved) {
  (void)u;
  store_calls++;
  store_mark = reserved;
  return 0;
}

static void test_tx_jump_persists(void) {
  struct sensei_secure node;
  struct sensei_secure_keys k = fixed_keys();
  struct sensei_secure_store st = {.tx = store_tx};
  const uint8_t pt[8] = {0};
  uint8_t env[64];

  CHECK(sensei_secure_init(&node, C, SENSEI_SECURE_ROLE_NODE, &k, 0, 0, 16, NULL, 0, &st) == 0);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 8, env, sizeof(env)) > 0);
  CHECK(store_calls == 1 && store_mark == 16 && node.tx_reserved == 16);
  CHECK(sensei_secure_tx_jump(&node, 640) == 0);
  CHECK(node.tx_reserved == 16); /* only the next seal moves the mark */
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 8, env, sizeof(env)) > 0);
  CHECK(store_calls == 2 && store_mark == 640 + 15 && node.tx_last == 640);
  sensei_secure_close(&node);
}

/* The reserved bit, generation bits without the ratchet bit, kinds: refused at the header. Generation 0 keeps its
 * bytes. */
static void test_kind_byte(void) {
  struct sensei_secure_info info;
  uint8_t e[40];
  const unsigned kinds[] = {1, 2, 3, 4};

  for (unsigned i = 0; i < 4; i++) {
    unsigned k = kinds[i];
    const bool down = k == 2 || k == 4;

    memset(e, 0, sizeof(e));
    e[0] = (uint8_t)(0xAC | (down ? 1u : 0u));
    e[1] = (uint8_t)k;
    e[2] = 1; /* key id 1 */
    e[2 + (down ? 4 : 0) + 4] = 1;
    if (down) {
      e[6] = 1; /* sender 1 */
    }
    CHECK(sensei_secure_peek(e, sizeof(e), &info) == 0 && info.kind == k && !info.ratchet && info.gen_bits == 0);
    for (unsigned gen = 0; gen < 4; gen++) {
      e[1] = (uint8_t)(0x80u | (gen << 4) | k);
      CHECK(sensei_secure_peek(e, sizeof(e), &info) == 0 && info.kind == k && info.ratchet && info.gen_bits == gen);
      CHECK(sensei_secure_kind(e) == k && sensei_secure_is_ratchet(e) && sensei_secure_gen_bits(e) == gen);
      e[1] |= 0x40;
      CHECK(sensei_secure_peek(e, sizeof(e), &info) == SENSEI_SECURE_ERR_FORMAT); /* the reserved bit */
      e[1] = (uint8_t)((gen << 4) | k);
      CHECK((gen == 0
                 ? sensei_secure_peek(e, sizeof(e), &info) == 0
                 : sensei_secure_peek(e, sizeof(e), &info) == SENSEI_SECURE_ERR_FORMAT)); /* bits but no ratchet bit */
    }
    e[1] =
        (uint8_t)(0x80u | 0x10u | (down ? (k == 2 ? 1u : 3u) : (k == 1 ? 2u : 4u))); /* a kind of the wrong direction */
    CHECK(sensei_secure_peek(e, sizeof(e), &info) == SENSEI_SECURE_ERR_FORMAT);
  }
  e[1] = 0x80u | 0x10u; /* kind 0 */
  e[0] = 0xAC;
  CHECK(sensei_secure_peek(e, sizeof(e), &info) == SENSEI_SECURE_ERR_FORMAT);
  e[1] = 0x80u | 0x10u | 5u;
  CHECK(sensei_secure_peek(e, sizeof(e), &info) == SENSEI_SECURE_ERR_FORMAT);
  CHECK(sensei_secure_peek(e, 10, &info) == SENSEI_SECURE_ERR_FORMAT);
}

/* ---- 6. scripted transcript ---- */

struct model {
  struct sensei_secure node, back;
  unsigned n_gen; /* node: 0, or 1 after the switch */
  struct sensei_ratchet_chain n_chain;
  uint8_t n_kdown[16];
  bool n_prev; /* node: generation 0 downlinks still accepted */
  bool b_promoted;
  bool b_prev; /* collector: generation 0 uplinks still accepted */
  struct sensei_ratchet_chain b_chain;
  uint8_t b_kdown[16];
  uint32_t seal_gen; /* generation of the next downlink */
};

static int n_tx(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  struct model *m = u;

  *gen = m->n_gen;
  return m->n_gen == 0 ? 0 : sensei_ratchet_chain_key(C, &m->n_chain, counter, true, SENSEI_RATCHET_MAX_SKIP, key);
}

static int n_rx(void *u, bool ratchet, unsigned bits, uint32_t counter, uint8_t key[16]) {
  struct model *m = u; /* cppcheck-suppress constVariablePointer */

  (void)counter;
  if (ratchet) {
    if (m->n_gen == 0 || bits != (m->n_gen & 3u)) {
      return SENSEI_SECURE_ERR_GEN;
    }
    memcpy(key, m->n_kdown, 16);
    return 0;
  }
  return m->n_gen == 0 || m->n_prev ? 1 : SENSEI_SECURE_ERR_GEN;
}

static int b_tx(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  struct model *m = u; /* cppcheck-suppress constVariablePointer */

  (void)counter;
  *gen = m->seal_gen;
  if (m->seal_gen != 0) {
    memcpy(key, m->b_kdown, 16);
  }
  return 0;
}

static int b_rx(void *u, bool ratchet, unsigned bits, uint32_t counter, uint8_t key[16]) {
  struct model *m = u;

  if (ratchet) { /* "next" before the promotion, "cur" after: the same chain */
    return bits == 1 ? sensei_ratchet_chain_key(C, &m->b_chain, counter, false, SENSEI_RATCHET_MAX_SKIP, key)
                     : SENSEI_SECURE_ERR_GEN;
  }
  return !m->b_promoted || m->b_prev ? 1 : SENSEI_SECURE_ERR_GEN;
}

/* After a ratchet uplink opened: promote and keep the chain at idx_lo = max(0, idx(highest) - 1). */
static void b_commit(struct model *m) {
  uint32_t high = sensei_secure_rx_highest(&m->back, 0);
  uint32_t ep = sensei_ratchet_epoch(high);

  m->b_promoted = true;
  if (ep >= m->b_chain.up_base) {
    uint32_t idx = ep - m->b_chain.up_base;
    uint32_t lo = idx > 0 ? idx - 1 : 0;

    if (lo > m->b_chain.idx) {
      CHECK(sensei_ratchet_chain_advance(C, &m->b_chain, lo, SENSEI_RATCHET_MAX_SKIP) == 0);
    }
  }
}

static void do_switch(struct model *m) {
  struct sensei_secure_keys k = fixed_keys();
  const vec_rat_mix_t *v = &vec_rat_mix[0]; /* the ephemeral keys of the transcript are those of the first mix */
  struct sensei_ratchet_mix nm, bm;
  uint8_t root0[32];

  CHECK(sensei_ratchet_root0(C, &k, root0) == 0 && memcmp(root0, vec_rat_root0, 32) == 0);
  CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, v->e_n_priv, root0, k.key_id, 1, VEC_RAT_UP_BASE_T, v->e_n_pub,
                           v->e_c_pub, &nm) == 0);
  CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_BACKEND, v->e_c_priv, root0, k.key_id, 1, VEC_RAT_UP_BASE_T,
                           v->e_n_pub, v->e_c_pub, &bm) == 0);
  CHECK(memcmp(&nm, &bm, sizeof(nm)) == 0);
  m->n_gen = 1;
  sensei_ratchet_chain_init(&m->n_chain, nm.ck, VEC_RAT_UP_BASE_T);
  memcpy(m->n_kdown, nm.k_down, 16);
  m->n_prev = true;
  sensei_ratchet_chain_init(&m->b_chain, bm.ck, VEC_RAT_UP_BASE_T);
  memcpy(m->b_kdown, bm.k_down, 16);
  m->b_promoted = false;
  m->b_prev = true;
}

static void test_transcript(void) {
  struct model m;
  uint8_t out[300], pt[300];

  memset(&m, 0, sizeof(m));
  open_session(&m.node, SENSEI_SECURE_ROLE_NODE, 0, 0);
  open_session(&m.back, SENSEI_SECURE_ROLE_BACKEND, VEC_RAT_SENDER, 0);
  sensei_secure_set_keyring(&m.node, &(struct sensei_secure_keyring){.user = &m, .tx_key = n_tx, .rx_key = n_rx});
  sensei_secure_set_keyring(&m.back, &(struct sensei_secure_keyring){.user = &m, .tx_key = b_tx, .rx_key = b_rx});
  for (int i = 0; i < VEC_RAT_STEPS_COUNT; i++) {
    const vec_rat_step_t *st = &vec_rat_steps[i];
    const vec_rat_step_t *ref = &vec_rat_steps[st->ref];
    size_t n = 0;
    int r;

    switch (st->op) {
    case VEC_RAT_OP_SEAL_N:
      CHECK(sensei_secure_tx_jump(&m.node, st->counter) == 0);
      r = sensei_secure_seal(&m.node, (enum sensei_secure_kind)st->kind, NULL, st->plain, st->plain_len, out,
                             sizeof(out));
      CHECK(r == (int)st->sealed_len && r > 0 && memcmp(out, st->sealed, st->sealed_len) == 0);
      break;
    case VEC_RAT_OP_SEAL_B:
      m.seal_gen = st->gen;
      CHECK(sensei_secure_tx_jump(&m.back, st->counter) == 0);
      r = sensei_secure_seal(&m.back, (enum sensei_secure_kind)st->kind, NULL, st->plain, st->plain_len, out,
                             sizeof(out));
      CHECK(r == (int)st->sealed_len && r > 0 && memcmp(out, st->sealed, st->sealed_len) == 0);
      break;
    case VEC_RAT_OP_SWITCH:
      do_switch(&m);
      break;
    case VEC_RAT_OP_OPEN_B:
      r = sensei_secure_open(&m.back, 0, NULL, ref->sealed, ref->sealed_len, pt, sizeof(pt), &n, NULL);
      CHECK(r == st->expect);
      if (r == 0) {
        CHECK(n == ref->plain_len && memcmp(pt, ref->plain, n) == 0);
        if (ref->gen != 0) {
          b_commit(&m);
        }
      }
      break;
    case VEC_RAT_OP_OPEN_N:
      r = sensei_secure_open(&m.node, 0, NULL, ref->sealed, ref->sealed_len, pt, sizeof(pt), &n, NULL);
      CHECK(r == st->expect);
      if (r == 0) {
        CHECK(n == ref->plain_len && memcmp(pt, ref->plain, n) == 0);
      }
      break;
    case VEC_RAT_OP_DROP_PREV:
      m.n_prev = false;
      m.b_prev = false;
      break;
    default:
      CHECK(0);
    }
    if (check_failures) {
      printf("transcript step %d failed\n", i);
      break;
    }
  }
  /* the node of generation 1 refuses a legacy uplink-direction key too: after the previous generation is dropped, a
   * generation 0 envelope is refused at the hook, whatever its counter */
  sensei_secure_close(&m.node);
  sensei_secure_close(&m.back);
}

/* ---- property: no (key, nonce) twice ---- */

#define PROP_UP 100000u /* uplinks */
#define PROP_MIXES 20u
#define PROP_DOWN 40000u /* downlinks of the 8 senders */
#define PROP_MAX (PROP_UP + PROP_DOWN)

struct rec {
  uint8_t kn[28];  /* key (16) then nonce (12) */
  uint8_t ratchet; /* the key is a message key (at most 64 counters) */
};

#define REC_KEY(r) ((r)->kn)
#define REC_NONCE(r) ((r)->kn + 16)

static struct rec prop_recs[PROP_MAX];

static uint32_t prng_state = 0x2545F491u;
static uint32_t prng(void) {
  prng_state ^= prng_state << 13;
  prng_state ^= prng_state >> 17;
  prng_state ^= prng_state << 5;
  return prng_state;
}

static int rec_cmp(const void *a, const void *b) { return memcmp(a, b, 28); /* key then nonce */ }

static void put32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void test_property(void) {
  struct sensei_secure_keys k = fixed_keys();
  uint8_t root[32], kd[PROP_MIXES + 1][16]; /* the downlink key of every generation */
  struct sensei_ratchet_chain chain;
  uint8_t mk[16];
  uint32_t gen = 0, counter = 0, up_base = 0, last_epoch_key = 0xFFFFFFFFu, dctr[8] = {0};
  size_t n = 0, mixes = 0, dups = 0;

  CHECK(sensei_ratchet_root0(C, &k, root) == 0);
  memcpy(kd[0], k.k_down, 16);
  memset(&chain, 0, sizeof(chain));
  for (uint32_t i = 0; i < PROP_UP; i++) {
    uint32_t r = prng();
    struct rec *rc = &prop_recs[n++];

    if (mixes < PROP_MIXES && i == (mixes + 1) * (PROP_UP / (PROP_MIXES + 1))) {
      /* a mix: new ephemeral keys on both sides, up_base above every epoch used so far, the counter jumps to it */
      uint8_t np[32], npub[32], cp[32], cpub[32];
      struct sensei_ratchet_mix a, b;

      up_base = sensei_ratchet_epoch(counter) + 1 + (prng() % 3);
      CHECK(C->x25519_keygen(C->ctx, np, npub) == 0 && C->x25519_keygen(C->ctx, cp, cpub) == 0);
      CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_NODE, np, root, k.key_id, gen + 1, up_base, npub, cpub, &a) == 0);
      CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_BACKEND, cp, root, k.key_id, gen + 1, up_base, npub, cpub, &b) ==
            0);
      CHECK(memcmp(&a, &b, sizeof(a)) == 0);
      gen++;
      mixes++;
      memcpy(root, a.root, 32); /* R_g is gone */
      memcpy(kd[gen], a.k_down, 16);
      sensei_ratchet_chain_init(&chain, a.ck, up_base);
      if (counter + 1 < 64 * up_base) {
        counter = 64 * up_base - 1;
      }
      last_epoch_key = 0xFFFFFFFFu;
    }
    counter++;
    if (gen >= 1 && r % 97 == 0) { /* a reboot or an old epoch: the next epoch starts (a multiple of 64) */
      counter = (counter + 63) & ~63u;
      if (r % 5 == 0) {
        counter += 64 * (r % 7);
      }
    } else if (gen == 0 && r % 97 == 0) {
      counter += r % 200;
    }
    if (gen == 0) {
      memcpy(REC_KEY(rc), k.k_up, 16);
      rc->ratchet = 0;
    } else {
      uint32_t ep = sensei_ratchet_epoch(counter);

      CHECK(ep >= up_base);
      if (ep != last_epoch_key) {
        CHECK(sensei_ratchet_chain_key(C, &chain, counter, true, SENSEI_RATCHET_MAX_SKIP, mk) == 0);
        last_epoch_key = ep;
      }
      memcpy(REC_KEY(rc), mk, 16);
      rc->ratchet = 1;
    }
    memset(REC_NONCE(rc), 0, 12); /* direction 0, sender 0, counter */
    put32le(REC_NONCE(rc) + 8, counter);
  }
  CHECK(mixes == PROP_MIXES && gen == PROP_MIXES);
  /* Downlinks: 8 senders, each with its own counter that goes on across the generations, under the static downlink
   * key of the generation that is current (the generations follow one another). */
  for (uint32_t i = 0; i < PROP_DOWN; i++) {
    unsigned s = prng() % 8;
    struct rec *rc = &prop_recs[n++];

    dctr[s] += 1 + (prng() % 3 == 0 ? prng() % 100 : 0);
    memcpy(REC_KEY(rc), kd[(uint64_t)i * (PROP_MIXES + 1) / PROP_DOWN], 16);
    rc->ratchet = 0;
    memset(REC_NONCE(rc), 0, 12);
    REC_NONCE(rc)[0] = 1;
    put32le(REC_NONCE(rc) + 4, 0x1000u + s);
    put32le(REC_NONCE(rc) + 8, dctr[s]);
  }
  qsort(prop_recs, n, sizeof(prop_recs[0]), rec_cmp);
  for (size_t i = 1; i < n; i++) {
    if (memcmp(prop_recs[i - 1].kn, prop_recs[i].kn, 28) == 0) {
      dups++;
    }
  }
  CHECK(dups == 0);
  /* an epoch key covers at most 64 counters */
  for (size_t i = 0, run = 0; i < n; i++) {
    run = i > 0 && memcmp(prop_recs[i - 1].kn, prop_recs[i].kn, 16) == 0 ? run + 1 : 1;
    if (prop_recs[i].ratchet) {
      CHECK(run <= SENSEI_RATCHET_EPOCH_SIZE);
    }
  }
  CHECK(n == PROP_MAX);
}

int main(void) {
  C = sensei_secure_crypto_openssl();
  test_root0();
  test_x25519();
  test_mix();
  test_chain();
  test_epoch_math();
  test_envelopes();
  test_envelope_tamper();
  test_keyring();
  test_tx_jump_persists();
  test_kind_byte();
  test_transcript();
  test_property();
  return CHECK_DONE();
}
