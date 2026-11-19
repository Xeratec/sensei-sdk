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

#define BSENDER 0x01020304u

/* A session. rx_hi: persisted highest counter of rx_sender (0: nothing persisted). The struct is filled with
 * garbage first: init must not read it. */
static void open_full(struct sensei_secure *s, enum sensei_secure_role role, uint32_t sender, uint32_t tx_res,
                      uint32_t rx_sender, uint32_t rx_hi, uint32_t step, const struct sensei_secure_store *store) {
  struct sensei_secure_keys k = keys_for(role);
  struct sensei_secure_rx_state rx = {rx_sender, rx_hi};

  memset(s, 0xA5, sizeof(*s));
  CHECK(sensei_secure_init(s, C, role, &k, sender, tx_res, step, rx_hi ? &rx : NULL, rx_hi ? 1 : 0, store) == 0);
}

/* Backend sessions seal as BSENDER and a node tracks the sender of the backend. */
static void open_session(struct sensei_secure *s, enum sensei_secure_role role, uint32_t tx_res, uint32_t rx_hi,
                         uint32_t step) {
  const int backend = role == SENSEI_SECURE_ROLE_BACKEND;

  open_full(s, role, backend ? BSENDER : 0, tx_res, backend ? 0 : BSENDER, rx_hi, step, NULL);
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

    open_full(&tx, is_up(v->kind) ? SENSEI_SECURE_ROLE_NODE : SENSEI_SECURE_ROLE_BACKEND, v->sender, v->counter - 1, 0,
              0, 1, NULL);
    open_full(&rx, is_up(v->kind) ? SENSEI_SECURE_ROLE_BACKEND : SENSEI_SECURE_ROLE_NODE, is_up(v->kind) ? BSENDER : 0,
              0, 0, 0, 1, NULL);
    m = sensei_secure_seal(&tx, (enum sensei_secure_kind)v->kind, v->has_node_id ? &v->node_id : NULL, v->plain,
                           v->plain_len, out, sizeof(out));
    CHECK(m == (int)v->sealed_len);
    CHECK(m == (int)sensei_secure_sealed_size(v->plain_len, v->has_node_id, !is_up(v->kind)));
    CHECK(m > 0 && memcmp(out, v->sealed, v->sealed_len) == 0);
    CHECK(sensei_secure_is_sealed(v->sealed, v->sealed_len));
    CHECK(sensei_secure_peek_key_id(v->sealed, v->sealed_len) == VEC_SEC_KEY_ID);
    CHECK(sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, &kind) == 0);
    CHECK(n == v->plain_len && (v->plain_len == 0 || memcmp(pt, v->plain, n) == 0) && kind == v->kind);
    CHECK(sensei_secure_rx_highest(&rx, v->sender) == v->counter);
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
    CHECK(rx.n_peers == 0); /* nothing was recorded */
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
    CHECK(sensei_secure_init(&other, C, SENSEI_SECURE_ROLE_BACKEND, &k, BSENDER, 0, 1, NULL, 0, NULL) == 0);
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
  CHECK(sensei_secure_rx_highest(&back, 0) == 100);
  sensei_secure_close(&node);
  sensei_secure_close(&back);
}

static struct {
  int tx_calls, rx_calls;
  uint32_t tx_last, rx_sender, rx_last;
  int fail, rx_fail;
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

static int p_rx(void *u, uint32_t sender, uint32_t h) {
  (void)u;
  if (st.rx_fail) {
    return -1;
  }
  st.rx_calls++;
  st.rx_sender = sender;
  st.rx_last = h;
  return 0;
}

static const struct sensei_secure_store STORE = {p_tx, p_rx, NULL};

static void test_counters(void) {
  struct sensei_secure node, back;
  uint8_t env[60], pt[60];
  uint8_t msg[4] = {9, 9, 9, 9};
  size_t n;
  int m;

  memset(&st, 0, sizeof(st));
  open_full(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 0, 0, 4, &STORE);
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
  open_full(&node, SENSEI_SECURE_ROLE_NODE, 0, st.tx_last, 0, 0, 4, &STORE);
  m = sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env));
  CHECK(m > 0 && node.tx_last == 13);
  open_session(&back, SENSEI_SECURE_ROLE_BACKEND, 0, 0, 1);
  CHECK(sensei_secure_open(&back, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_rx_highest(&back, 0) == 13);
  sensei_secure_close(&back);

  /* receive side of the node: persisted highest, window treated as seen after a reboot */
  {
    struct sensei_secure b2, n2;
    uint8_t e5[60], e11[60], e12[60];
    int l5 = 0, l11 = 0, l12 = 0;

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
    open_full(&n2, SENSEI_SECURE_ROLE_NODE, 0, 0, BSENDER, 11, 4, &STORE);
    st.rx_calls = 0;
    CHECK(sensei_secure_open(&n2, 0, NULL, e11, (size_t)l11, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
    CHECK(sensei_secure_open(&n2, 0, NULL, e5, (size_t)l5, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
    CHECK(sensei_secure_open(&n2, SENSEI_SECURE_KIND_DOWNLINK, NULL, e12, (size_t)l12, pt, sizeof(pt), &n, NULL) == 0);
    CHECK(st.rx_calls == 1 && st.rx_sender == BSENDER && st.rx_last == 12);

    /* a failing store refuses the message, leaves no plaintext, and the window does not move */
    sensei_secure_close(&n2);
    open_full(&n2, SENSEI_SECURE_ROLE_NODE, 0, 0, BSENDER, 4, 4, &STORE);
    st.rx_fail = 1;
    memset(pt, 0, sizeof(pt));
    CHECK(sensei_secure_open(&n2, 0, NULL, e12, (size_t)l12, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_STORE);
    for (size_t i = 0; i < 4; i++) {
      CHECK(pt[i] == 0);
    }
    CHECK(sensei_secure_rx_highest(&n2, BSENDER) == 4);
    st.rx_fail = 0;
    CHECK(sensei_secure_open(&n2, 0, NULL, e12, (size_t)l12, pt, sizeof(pt), &n, NULL) == 0); /* retry works */
    CHECK(sensei_secure_rx_highest(&n2, BSENDER) == 12);
    sensei_secure_close(&n2);
    sensei_secure_close(&b2);
  }

  /* counter exhausted */
  sensei_secure_close(&node);
  open_full(&node, SENSEI_SECURE_ROLE_NODE, 0, 0xFFFFFFFEu, 0, 0, 4, NULL);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) > 0);
  CHECK(sensei_secure_seal(&node, SENSEI_SECURE_KIND_UPLINK, NULL, msg, 4, env, sizeof(env)) ==
        SENSEI_SECURE_ERR_STATE);
  sensei_secure_close(&node);
}

/* Two senders use the same counters: different nonces, each has its own window on the node. */
static void test_senders(void) {
  struct sensei_secure a, b, node;
  uint8_t ea[8][60], eb[8][60], pt[60], msg[4] = {1, 2, 3, 4};
  int la[8], lb[8];
  size_t n;

  open_full(&a, SENSEI_SECURE_ROLE_BACKEND, 0xAAAA0001u, 0, 0, 0, 1, NULL);
  open_full(&b, SENSEI_SECURE_ROLE_BACKEND, 0xBBBB0002u, 0, 0, 0, 1, NULL);
  open_full(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 0, 0, 1, NULL);
  for (int i = 0; i < 8; i++) {
    la[i] = sensei_secure_seal(&a, SENSEI_SECURE_KIND_DOWNLINK, NULL, msg, 4, ea[i], sizeof(ea[i]));
    lb[i] = sensei_secure_seal(&b, SENSEI_SECURE_KIND_DOWNLINK, NULL, msg, 4, eb[i], sizeof(eb[i]));
    CHECK(la[i] == 4 + 30 && lb[i] == la[i]);
    /* same key, same counter, same plaintext: different sender, different nonce, different ciphertext */
    CHECK(memcmp(ea[i], eb[i], (size_t)la[i]) != 0);
    CHECK(memcmp(ea[i] + 14, eb[i] + 14, 4 + 4) != 0); /* ciphertext and tag */
  }
  /* counter 1 of both is accepted, a replay of either is not, counters of one do not block the other */
  CHECK(sensei_secure_open(&node, 0, NULL, ea[0], (size_t)la[0], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_open(&node, 0, NULL, eb[0], (size_t)lb[0], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_open(&node, 0, NULL, ea[0], (size_t)la[0], pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
  CHECK(sensei_secure_open(&node, 0, NULL, eb[0], (size_t)lb[0], pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_REPLAY);
  CHECK(sensei_secure_open(&node, 0, NULL, ea[5], (size_t)la[5], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_open(&node, 0, NULL, eb[1], (size_t)lb[1], pt, sizeof(pt), &n, NULL) == 0);
  CHECK(sensei_secure_rx_highest(&node, 0xAAAA0001u) == 6 && sensei_secure_rx_highest(&node, 0xBBBB0002u) == 2);
  /* the sender field is covered by the tag: a message cannot be moved to another sender's window */
  {
    uint8_t forged[60];

    memcpy(forged, eb[3], (size_t)lb[3]);
    memcpy(forged + 6, ea[3] + 6, 4); /* the sender field */
    CHECK(sensei_secure_open(&node, 0, NULL, forged, (size_t)lb[3], pt, sizeof(pt), &n, NULL) ==
          SENSEI_SECURE_ERR_AUTH);
  }
  /* sender 0 in a downlink is not an envelope */
  {
    uint8_t zero[60];

    memcpy(zero, ea[2], (size_t)la[2]);
    memset(zero + 6, 0, 4);
    CHECK(sensei_secure_open(&node, 0, NULL, zero, (size_t)la[2], pt, sizeof(pt), &n, NULL) ==
          SENSEI_SECURE_ERR_FORMAT);
  }
  sensei_secure_close(&a);
  sensei_secure_close(&b);
  sensei_secure_close(&node);
}

/* The table holds SENSEI_SECURE_MAX_SENDERS senders; the next one is refused (also a forged one cannot take a slot). */
static void test_sender_table(void) {
  struct sensei_secure node, back;
  uint8_t env[60], pt[60], msg[4] = {1, 2, 3, 4};
  size_t n;
  int m;

  open_full(&node, SENSEI_SECURE_ROLE_NODE, 0, 0, 0, 0, 1, NULL);
  for (uint32_t i = 1; i <= SENSEI_SECURE_MAX_SENDERS + 1; i++) {
    open_full(&back, SENSEI_SECURE_ROLE_BACKEND, 0x1000u + i, 0, 0, 0, 1, NULL);
    m = sensei_secure_seal(&back, SENSEI_SECURE_KIND_DOWNLINK, NULL, msg, 4, env, sizeof(env));
    if (i <= SENSEI_SECURE_MAX_SENDERS) {
      CHECK(sensei_secure_open(&node, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);
    } else {
      CHECK(sensei_secure_open(&node, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_SENDER);
    }
    sensei_secure_close(&back);
  }
  CHECK(node.n_peers == SENSEI_SECURE_MAX_SENDERS);
  /* a known sender still works */
  open_full(&back, SENSEI_SECURE_ROLE_BACKEND, 0x1001u, 1, 0, 0, 1, NULL);
  m = sensei_secure_seal(&back, SENSEI_SECURE_KIND_DOWNLINK, NULL, msg, 4, env, sizeof(env));
  CHECK(sensei_secure_open(&node, 0, NULL, env, (size_t)m, pt, sizeof(pt), &n, NULL) == 0);
  sensei_secure_close(&back);
  sensei_secure_close(&node);
}

/* init: bad arguments, and the struct is never read before it is cleared. */
static void test_init(void) {
  struct sensei_secure s;
  struct sensei_secure_keys k = keys_for(SENSEI_SECURE_ROLE_BACKEND);
  struct sensei_secure_rx_state dup[2] = {{5, 1}, {5, 2}};

  memset(&s, 0xFF, sizeof(s));
  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_BACKEND, &k, 0, 0, 1, NULL, 0, NULL) == SENSEI_SECURE_ERR_STATE);
  CHECK(!s.open && s.n_peers == 0 && s.store.tx == NULL && s.store.rx == NULL); /* cleared, not trusted */
  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_NODE, &k, 7, 0, 1, NULL, 0, NULL) == SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_NODE, &k, 0, 0, 1, dup, 2, NULL) == SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_NODE, &k, 0, 0, 1, NULL, SENSEI_SECURE_MAX_SENDERS + 1, NULL) ==
        SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_NODE, &k, 0, 0, 0, NULL, 0, NULL) == SENSEI_SECURE_ERR_STATE);
}

/* Plain protobuf never looks sealed: Settings patches that start with field 20, 21, 36, 37, 52 or 53 (varints) start
 * with 0xA0 or 0xA8. The version bits keep them apart. */
static void test_is_sealed_markers(void) {
  uint8_t buf[60];

  memset(buf, 0, sizeof(buf));
  for (unsigned first = 0; first < 256; first++) {
    buf[0] = (uint8_t)first;
    CHECK(sensei_secure_is_sealed(buf, sizeof(buf)) == (first >= 0xAC && first <= 0xAF));
  }
  buf[0] = 0xA0; /* uplink_s, camera_stream, sound_alert_db */
  buf[1] = 0x2C;
  CHECK(!sensei_secure_is_sealed(buf, sizeof(buf)));
  buf[0] = 0xA8; /* radio, jpeg_quality, sound_bands */
  buf[1] = 0x01;
  CHECK(!sensei_secure_is_sealed(buf, sizeof(buf)));
  /* an envelope of the old version (1: 0xA4 ... 0xA7) is refused */
  buf[0] = 0xA4;
  CHECK(!sensei_secure_is_sealed(buf, sizeof(buf)));
  CHECK(sensei_secure_peek_key_id(buf, sizeof(buf)) == 0);
  /* too short */
  buf[0] = 0xAC;
  CHECK(!sensei_secure_is_sealed(buf, SENSEI_SECURE_OVERHEAD - 1));
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

/* Generation 0 is the envelope as it was; the kind byte keeps its meaning and the generation bits are refused where
 * there is no key for them. */
static void test_generation_bits(void) {
  for (int i = 0; i < VEC_SEC_COUNT; i++) {
    const vec_sec_t *v = &vec_sec[i];
    struct sensei_secure rx;
    struct sensei_secure_info info;
    uint8_t bad[2200], pt[2200];
    size_t n;
    int m;

    /* generation 0: byte 1 is the kind, byte for byte as before */
    CHECK(v->sealed[1] == v->kind && sensei_secure_kind(v->sealed) == v->kind);
    CHECK(!sensei_secure_is_ratchet(v->sealed) && sensei_secure_gen_bits(v->sealed) == 0);
    CHECK(sensei_secure_peek(v->sealed, v->sealed_len, &info) == 0 && info.kind == v->kind && !info.ratchet &&
          info.gen_bits == 0 && info.counter == v->counter && info.sender == v->sender &&
          info.key_id == VEC_SEC_KEY_ID && info.to_node == !is_up(v->kind) && info.has_node_id == (bool)v->has_node_id);
    CHECK(!v->has_node_id || info.node_id == v->node_id);

    open_session(&rx, is_up(v->kind) ? SENSEI_SECURE_ROLE_BACKEND : SENSEI_SECURE_ROLE_NODE, 0, 0, 1);
    memcpy(bad, v->sealed, v->sealed_len);
    bad[1] = (uint8_t)(v->kind | SENSEI_SECURE_KIND_RESERVED); /* the reserved bit */
    CHECK(sensei_secure_open(&rx, 0, NULL, bad, v->sealed_len, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(sensei_secure_peek_key_id(bad, v->sealed_len) == 0);
    bad[1] = (uint8_t)(v->kind | 0x10u); /* generation bits without the ratchet bit */
    CHECK(sensei_secure_open(&rx, 0, NULL, bad, v->sealed_len, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_FORMAT);
    bad[1] = (uint8_t)(v->kind | SENSEI_SECURE_KIND_RATCHET | 0x10u); /* a ratchet envelope: no keyring, no key */
    CHECK(sensei_secure_open(&rx, 0, NULL, bad, v->sealed_len, pt, sizeof(pt), &n, NULL) == SENSEI_SECURE_ERR_GEN);
    CHECK(sensei_secure_peek_key_id(bad, v->sealed_len) == VEC_SEC_KEY_ID); /* still readable for the lookup */
    CHECK(rx.n_peers == 0);
    m = sensei_secure_open(&rx, 0, NULL, v->sealed, v->sealed_len, pt, sizeof(pt), &n, NULL);
    CHECK(m == 0 && n == v->plain_len);
    sensei_secure_close(&rx);
  }
}

int main(void) {
  C = sensei_secure_crypto_openssl();
  test_keys();
  test_vectors();
  test_tamper();
  test_errors();
  test_replay_window();
  test_counters();
  test_senders();
  test_sender_table();
  test_init();
  test_is_sealed_markers();
  test_fresh_keys();
  test_generation_bits();
  return CHECK_DONE();
}
