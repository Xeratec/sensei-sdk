/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Envelope tests with the OpenSSL backend: vectors from the Python implementation (both
 * directions), tampering, replay, key id, counters. */

#include <string.h>

#include "check.h"
#include "sensei_secure.h"
#include "vectors.h"

static const struct sensei_secure_crypto *C;

static sensei_secure_key_t import(const uint8_t *priv) {
  sensei_secure_key_t k = 0;

  CHECK(C->ecdh_import(C->ctx, priv, 32, &k) == 0);
  return k;
}

static struct sensei_secure_keys keys_for(enum sensei_secure_role role) {
  struct sensei_secure_keys k;
  sensei_secure_key_t own = import(role == SENSEI_SECURE_ROLE_NODE ? vec_sec_node_priv : vec_sec_backend_priv);

  CHECK(sensei_secure_derive(C, own, role, vec_sec_node_pub, vec_sec_backend_pub, &k) == 0);
  C->key_destroy(C->ctx, own);
  return k;
}

static void open_session(struct sensei_secure *s, enum sensei_secure_role role, uint32_t tx_res, uint32_t rx_hi,
                         uint32_t step) {
  struct sensei_secure_keys k = keys_for(role);

  memset(s, 0, sizeof(*s));
  CHECK(sensei_secure_init(s, C, role, &k, tx_res, rx_hi, step) == 0);
}

static void test_keys(void) {
  struct sensei_secure_keys kn = keys_for(SENSEI_SECURE_ROLE_NODE);
  struct sensei_secure_keys kb = keys_for(SENSEI_SECURE_ROLE_BACKEND);
  uint8_t pub[65];
  sensei_secure_key_t k = import(vec_sec_node_priv);

  CHECK(C->ecdh_public(C->ctx, k, pub) == 0 && memcmp(pub, vec_sec_node_pub, 65) == 0);
  C->key_destroy(C->ctx, k);
  CHECK(memcmp(kn.k_up, vec_sec_k_up, 16) == 0 && memcmp(kn.k_down, vec_sec_k_down, 16) == 0);
  CHECK(memcmp(kb.k_up, vec_sec_k_up, 16) == 0 && memcmp(kb.k_down, vec_sec_k_down, 16) == 0);
  CHECK(kn.key_id == VEC_SEC_KEY_ID && kb.key_id == VEC_SEC_KEY_ID);
}

static int is_up(unsigned kind) { return kind == SENSEI_SECURE_KIND_UPLINK || kind == SENSEI_SECURE_KIND_NODE_TO_HOST; }

/* Seal on one side must equal the Python bytes, the other side must open them. */
static void test_vectors(void) {
  for (int i = 0; i < VEC_SEC_COUNT; i++) {
    const vec_sec_t *v = &vec_sec[i];
    struct sensei_secure tx, rx;
    uint8_t out[2200], pt[2200];
    size_t n = 0;
    enum sensei_secure_kind kind = 0;
    int m;

    open_session(&tx, is_up(v->kind) ? SENSEI_SECURE_ROLE_NODE : SENSEI_SECURE_ROLE_BACKEND, v->counter - 1, 0, 1);
    open_session(&rx, is_up(v->kind) ? SENSEI_SECURE_ROLE_BACKEND : SENSEI_SECURE_ROLE_NODE, 0, 0, 1);
    m = sensei_secure_seal(&tx, (enum sensei_secure_kind)v->kind, v->has_node_id ? &v->node_id : NULL, v->plain,
                           v->plain_len, out, sizeof(out));
    CHECK(m == (int)v->sealed_len);
    CHECK(m == (int)sensei_secure_sealed_size(v->plain_len, v->has_node_id));
    CHECK(m > 0 && memcmp(out, v->sealed, v->sealed_len) == 0);
    CHECK(sensei_secure_is_sealed(v->sealed, v->sealed_len));
    CHECK(sensei_secure_peek_key_id(v->sealed, v->sealed_len) == VEC_SEC_KEY_ID);
    CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, &kind) == 0);
    CHECK(n == v->plain_len && (v->plain_len == 0 || memcmp(pt, v->plain, n) == 0) && kind == v->kind);
    CHECK(rx.rx.highest == v->counter);
    sensei_secure_close(&tx);
    sensei_secure_close(&rx);
  }
}

static void test_tamper(void) {
  for (int i = 0; i < VEC_SEC_COUNT; i++) {
    const vec_sec_t *v = &vec_sec[i];
    struct sensei_secure rx;
    uint8_t bad[2200], pt[2200];
    size_t n;

    open_session(&rx, is_up(v->kind) ? SENSEI_SECURE_ROLE_BACKEND : SENSEI_SECURE_ROLE_NODE, 0, 0, 1);
    for (size_t b = 0; b < v->sealed_len; b++) {
      memcpy(bad, v->sealed, v->sealed_len);
      bad[b] ^= 0x01;
      CHECK(sensei_secure_open(&rx, 0, NULL, bad, v->sealed_len, pt, sizeof(pt), &n, NULL) < 0);
    }
    CHECK(rx.rx.highest == 0 && rx.rx.bitmap == 0); /* nothing was recorded */
    CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len - 1, pt, sizeof(pt), &n, NULL) < 0);
    CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, 5, pt, sizeof(pt), &n, NULL) < 0);
    CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, NULL) == 0);
    sensei_secure_close(&rx);
  }
}

static void test_errors(void) {
  struct sensei_secure node, back, other;
  uint8_t env[300], pt[300], plain_uplink[] = {0x7A, 0x00, 0x08, 0x01};
  size_t n;
  int m;
  const uint64_t id = 7, wrong = 8;

  open_session(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 4);
  open_session(&back, SENSEI_SECURE_ROLE_BACKEND, 0, 0, 4);

  /* direction and kind */
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_DOWNLINK, NULL, pt, 4, env, sizeof(env)) ==
        SENSEI_SECURE_ERR_KIND);
  CHECK(sensei_secure_seal(&back, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 4, env, sizeof(env)) == SENSEI_SECURE_ERR_KIND);
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 20, env, sizeof(env));
  CHECK(m == 20 + 26);
  CHECK(sensei_secure_open(&node, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_FORMAT);
  CHECK(sensei_secure_open(&back, SENSEI_SECURE_KIND_NODE_TO_HOST, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_KIND);
  CHECK(sensei_secure_open(&back, SENSEI_SECURE_KIND_UPLINK, NULL, env, (size_t)m, pt, 10, &n, NULL) ==
        SENSEI_SECURE_ERR_SPACE);
  CHECK(sensei_secure_open(&back, SENSEI_SECURE_KIND_UPLINK, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);
  CHECK(n == 20);
  /* replay of the same bytes */
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);

  /* too small output */
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 20, env, 45) == SENSEI_SECURE_ERR_SPACE);

  /* wrong key id: a session of another key pair */
  {
    uint8_t priv[32], pub[65];
    struct sensei_secure_keys k;
    sensei_secure_key_t own;

    CHECK(sensei_secure_keygen(C, priv, pub) == 0);
    own = import(priv);
    CHECK(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_NODE, pub, vec_sec_backend_pub, &k) == 0);
    C->key_destroy(C->ctx, own);
    memset(&other, 0, sizeof(other));
    CHECK(sensei_secure_init(&other, C, SENSEI_SECURE_ROLE_BACKEND, &k, 0, 0, 1) == 0);
    CHECK(k.key_id != VEC_SEC_KEY_ID);
    m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, pt, 3, env, sizeof(env));
    CHECK(sensei_secure_open(&other, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_KEY_ID);
    sensei_secure_close(&other);
  }

  /* node id in the envelope */
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, &id, pt, 3, env, sizeof(env));
  CHECK(m == 3 + 34);
  CHECK(sensei_secure_open(&back, 0, &wrong, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_NODE);
  CHECK(sensei_secure_open(&back, 0, &id, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);

  /* plain protobuf is not an envelope */
  CHECK(!sensei_secure_is_sealed(plain_uplink, sizeof(plain_uplink)));
  CHECK(sensei_secure_open(&back, 0, NULL, plain_uplink, sizeof(plain_uplink), pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_FORMAT);
  {
    uint8_t plain[40];

    memset(plain, 0x0A, sizeof(plain)); /* a field 1 tag: plain Uplinks start below 0x80 */
    CHECK(!sensei_secure_is_sealed(plain, sizeof(plain)));
  }
  sensei_secure_close(&node);
  sensei_secure_close(&back);
}

static void test_replay_window(void) {
  struct sensei_secure node, back;
  uint8_t env[100][40], pt[40];
  int len[100];
  size_t n;
  uint8_t msg[4] = {1, 2, 3, 4};

  open_session(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 16);
  open_session(&back, SENSEI_SECURE_ROLE_BACKEND, 0, 0, 1);
  for (int i = 0; i < 100; i++) {
    len[i] = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, sizeof(msg), env[i], sizeof(env[i]));
    CHECK(len[i] == 30); /* env[i] carries counter i + 1 */
  }
  /* out of order inside the window */
  const int order[] = {4, 2, 3, 0, 1};

  for (int i = 0; i < 5; i++) {
    CHECK(sensei_secure_open(&back, 0, NULL, env[order[i]], (size_t)len[order[i]], pt, sizeof(pt), &n, NULL) == 0);
  }
  for (int i = 0; i < 5; i++) {
    CHECK(sensei_secure_open(&back, 0, NULL, env[i], (size_t)len[i], pt, sizeof(pt), &n, NULL) ==
          SENSEI_SECURE_ERR_REPLAY);
  }
  /* jump to 100: counters 37 ... 99 stay in the window, 36 and older are too old */
  CHECK(sensei_secure_open(&back, 0, NULL, env[99], (size_t)len[99], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_open(&back, 0, NULL, env[35], (size_t)len[35], pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_REPLAY); /* counter 36: 64 behind */
  CHECK(sensei_secure_open(&back, 0, NULL, env[36], (size_t)len[36], pt, sizeof(pt), &n, NULL) == 0); /* counter 37 */
  CHECK(sensei_secure_open(&back, 0, NULL, env[36], (size_t)len[36], pt, sizeof(pt), &n, NULL) ==
        SENSEI_SECURE_ERR_REPLAY);
  CHECK(sensei_secure_open(&back, 0, NULL, env[50], (size_t)len[50], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(back.rx.highest == 100);
  sensei_secure_close(&node);
  sensei_secure_close(&back);
}

static struct {
  int tx_calls, rx_calls;
  uint32_t tx_last, rx_last;
  int fail;
} st;

static int p_tx(void *u, uint32_t r) {
  (void)u;
  if (st.fail) {
    return -1;
  }
  st.tx_calls++;
  st.tx_last = r;
  return 0;
}

static void p_rx(void *u, uint32_t h) {
  (void)u;
  st.rx_calls++;
  st.rx_last = h;
}

static void test_counters(void) {
  struct sensei_secure node, back;
  uint8_t env[60], pt[60];
  uint8_t msg[4] = {9, 9, 9, 9};
  size_t n;
  int m;

  memset(&st, 0, sizeof(st));
  memset(&node, 0, sizeof(node));
  memset(&back, 0, sizeof(back));
  node.persist_tx = p_tx;
  node.persist_rx = p_rx;
  open_session(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 1);
  node.persist_tx = p_tx;
  node.persist_rx = p_rx;
  node.ctr_step = 4;
  for (int i = 1; i <= 9; i++) {
    m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, sizeof(msg), env, sizeof(env));
    CHECK(m > 0 && node.tx_last == (uint32_t)i);
  }
  CHECK(st.tx_calls == 3 && st.tx_last == 12 && node.tx_reserved == 12); /* 1..4, 5..8, 9..12 */

  /* persist failure: the counter is not used */
  st.fail = 1;
  for (int i = 0; i < 3; i++) { /* 10, 11, 12 need no write */
    CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) > 0);
  }
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) ==
        SENSEI_SECURE_ERR_STORE);
  CHECK(node.tx_last == 12);
  st.fail = 0;

  /* reboot: resume above the stored high-water mark */
  sensei_secure_close(&node);
  open_session(&node, SENSEI_SECURE_ROLE_NODE, st.tx_last, 0, 4);
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env));
  CHECK(m > 0 && node.tx_last == 13);
  open_session(&back, SENSEI_SECURE_ROLE_BACKEND, 0, 12, 1);
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0 && back.rx.highest == 13);
  sensei_secure_close(&back);

  /* receive side of the node: persisted highest, window treated as seen after a reboot */
  {
    struct sensei_secure b2, n2;
    uint8_t e5[60], e11[60], e12[60];
    int l5, l11, l12;

    open_session(&b2, SENSEI_SECURE_ROLE_BACKEND, 0, 0, 1);
    for (int i = 1; i <= 12; i++) {
      int l = sensei_secure_seal(&b2, SENSEI_SECURE_KIND_DOWNLINK, NULL, msg, 4, env, sizeof(env));

      if (i == 5) {
        memcpy(e5, env, (size_t)l);
        l5 = l;
      } else if (i == 11) {
        memcpy(e11, env, (size_t)l);
        l11 = l;
      } else if (i == 12) {
        memcpy(e12, env, (size_t)l);
        l12 = l;
      }
    }
    open_session(&n2, SENSEI_SECURE_ROLE_NODE, 0, 11, 4);
    n2.persist_rx = p_rx;
    st.rx_calls = 0;
    CHECK(sensei_secure_open(&n2, 0, NULL, e11, (size_t)l11, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
    CHECK(sensei_secure_open(&n2, 0, NULL, e5, (size_t)l5, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
    CHECK(sensei_secure_open(&n2, SENSEI_SECURE_KIND_DOWNLINK, NULL, e12, (size_t)l12, pt, sizeof(pt), &n, NULL) == 0);
    CHECK(st.rx_calls == 1 && st.rx_last == 12);
    sensei_secure_close(&n2);
    sensei_secure_close(&b2);
  }

  /* counter exhausted */
  sensei_secure_close(&node);
  open_session(&node, SENSEI_SECURE_ROLE_NODE, 0xFFFFFFFEu, 0, 4);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) > 0);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) ==
        SENSEI_SECURE_ERR_STATE);
  sensei_secure_close(&node);
}

/* Random key pairs on both sides agree. */
static void test_fresh_keys(void) {
  uint8_t np[32], npub[65], bp[32], bpub[65];
  struct sensei_secure_keys kn, kb;
  sensei_secure_key_t a, b;

  CHECK(sensei_secure_keygen(C, np, npub) == 0 && sensei_secure_keygen(C, bp, bpub) == 0);
  CHECK(npub[0] == 4 && bpub[0] == 4 && memcmp(np, bp, 32) != 0);
  a = import(np);
  b = import(bp);
  CHECK(sensei_secure_derive(C, a, SENSEI_SECURE_ROLE_NODE, npub, bpub, &kn) == 0);
  CHECK(sensei_secure_derive(C, b, SENSEI_SECURE_ROLE_BACKEND, npub, bpub, &kb) == 0);
  CHECK(memcmp(&kn, &kb, sizeof(kn)) == 0);
  /* a point that is not on the curve is refused */
  bpub[64] ^= 1;
  CHECK(sensei_secure_derive(C, a, SENSEI_SECURE_ROLE_NODE, npub, bpub, &kn) != 0);
  C->key_destroy(C->ctx, a);
  C->key_destroy(C->ctx, b);
}

int main(void) {
  C = sensei_secure_crypto_openssl();
  test_keys();
  test_vectors();
  test_tamper();
  test_errors();
  test_replay_window();
  test_counters();
  test_fresh_keys();
  return CHECK_DONE();
}
