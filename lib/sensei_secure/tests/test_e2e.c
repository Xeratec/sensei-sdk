/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Device side of the end-to-end security (src/sensei_e2e.c) with the OpenSSL backend and a fake flash. */

#include <errno.h>
#include <string.h>

#include "check.h"
#include "sensei_e2e.h"

static const struct sensei_secure_crypto *C;

/* fake flash */
static struct sensei_e2e_nv flash;
static int erased, fail_tx, fail_rx, fail_floor, fail_priv;

#define SENDER_A 0x0A0A0A0Au
#define SENDER_B 0x0B0B0B0Bu

static int f_priv(void *u, const uint8_t *p) {
  (void)u;
  if (fail_priv) {
    return -EIO;
  }
  memcpy(flash.priv, p, sizeof(flash.priv));
  flash.have_priv = true;
  return 0;
}
static int f_backend(void *u, const uint8_t *p) {
  (void)u;
  memcpy(flash.backend_pub, p, sizeof(flash.backend_pub));
  flash.have_backend = true;
  return 0;
}
static int f_tx(void *u, uint32_t r) {
  (void)u;
  if (fail_tx) {
    return -EIO;
  }
  flash.tx_reserved = r;
  return 0;
}
static int f_rx(void *u, const struct sensei_secure_rx_state *tab, size_t n) {
  (void)u;
  if (fail_rx) {
    return -EIO;
  }
  memcpy(flash.rx, tab, n * sizeof(*tab));
  flash.n_rx = n;
  return 0;
}
static void f_erase(void *u) {
  (void)u;
  memset(&flash, 0, sizeof(flash));
  erased++;
}
static int f_floor(void *u, uint32_t f) {
  (void)u;
  if (fail_floor) {
    return -EIO;
  }
  flash.rx_floor = f;
  return 0;
}
static const struct sensei_e2e_ops ops = {f_priv, f_backend, f_tx, f_rx, f_erase, f_floor};

/* the collector side */
struct backend {
  uint8_t priv[32], pub[65];
  struct sensei_secure s;
};

static uint32_t rx_of(const struct sensei_e2e_nv *nv, uint32_t sender) {
  for (size_t i = 0; i < nv->n_rx; i++) {
    if (nv->rx[i].sender == sender) {
      return nv->rx[i].highest;
    }
  }
  return 0;
}

static void backend_new(struct backend *b) { CHECK(sensei_secure_keygen(C, b->priv, b->pub) == 0); }

static void backend_session_as(struct backend *b, const uint8_t *node_pub, uint32_t sender) {
  struct sensei_secure_keys keys;
  sensei_secure_key_t own = 0;

  CHECK(C->ecdh_import(C->ctx, b->priv, 32, &own) == 0);
  CHECK(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_BACKEND, node_pub, b->pub, &keys) == 0);
  C->key_destroy(C->ctx, own);
  memset(&b->s, 0, sizeof(b->s));
  CHECK(sensei_secure_init(&b->s, C, SENSEI_SECURE_ROLE_BACKEND, &keys, sender, 0, 1, NULL, 0, NULL) == 0);
}

/* A sender that goes on from a counter (what the key store does: one counter per node). */
static void backend_session_from(struct backend *b, const uint8_t *node_pub, uint32_t sender, uint32_t reserved) {
  struct sensei_secure_keys keys;
  sensei_secure_key_t own = 0;

  CHECK(C->ecdh_import(C->ctx, b->priv, 32, &own) == 0);
  CHECK(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_BACKEND, node_pub, b->pub, &keys) == 0);
  C->key_destroy(C->ctx, own);
  memset(&b->s, 0, sizeof(b->s));
  CHECK(sensei_secure_init(&b->s, C, SENSEI_SECURE_ROLE_BACKEND, &keys, sender, reserved, 1, NULL, 0, NULL) == 0);
}

static void backend_session(struct backend *b, const uint8_t *node_pub) { backend_session_as(b, node_pub, SENDER_A); }

static void test_first_start(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  uint8_t pt[60], env[120], out[120];
  size_t n;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  CHECK(flash.have_priv && !sensei_e2e_ready(&e) && sensei_e2e_key_id(&e) == 0 && e.node_pub[0] == 4);
  /* Not provisioned: nothing can be sealed or opened. */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 10, env, sizeof(env)) == -ENOTCONN);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, 40, out, sizeof(out), &n) == -ENOTCONN);

  /* A second start with the stored key gives the same public key. */
  {
    struct sensei_e2e e2;
    struct sensei_e2e_nv nv = flash;

    CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 4) == 0);
    CHECK(memcmp(e2.node_pub, e.node_pub, 65) == 0);
  }
}

static void test_provision_and_traffic(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t pt[80], env[160], out[160], dl[40];
  size_t n;
  int m;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  for (size_t i = 0; i < sizeof(pt); i++) {
    pt[i] = (uint8_t)(i * 3);
  }
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 64) == -EINVAL);
  {
    uint8_t bad[65];

    memcpy(bad, b.pub, 65);
    bad[0] = 2;
    CHECK(sensei_e2e_provision(&e, bad, 65) == -EINVAL);
    memcpy(bad, b.pub, 65);
    bad[40] ^= 1; /* off the curve */
    CHECK(sensei_e2e_provision(&e, bad, 65) == -EINVAL);
    CHECK(!sensei_e2e_ready(&e) && !flash.have_backend);
  }
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0 && sensei_e2e_ready(&e) && flash.have_backend);
  backend_session(&b, e.node_pub);
  CHECK(sensei_e2e_key_id(&e) == b.s.key_id && sensei_e2e_key_id(&e) != 0);

  /* node to backend: uplink and host data, counters 1 and 2, flash written once per 4 */
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
  CHECK(m == (int)(sizeof(pt) + 26) && flash.tx_reserved == 4);
  CHECK(sensei_secure_open(&b.s, SENSEI_SECURE_KIND_UPLINK, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0);
  CHECK(n == sizeof(pt) && memcmp(out, pt, n) == 0);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_NODE_TO_HOST, pt, 5, env, sizeof(env));
  CHECK(m == 5 + 26 && flash.tx_reserved == 4);
  CHECK(sensei_secure_open(&b.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0 &&
        sensei_secure_rx_highest(&b.s, 0) == 2);
  /* a downlink kind cannot be sealed by the node */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_DOWNLINK, pt, 5, env, sizeof(env)) == -EACCES);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, 20) == -ENOSPC);
  /* flash failure: sending stops at the reserved limit, nothing is sent without a stored mark */
  for (int i = 0; i < 2; i++) {
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env)) > 0);
  }
  fail_tx = 1;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env)) == -EIO);
  fail_tx = 0;

  /* backend to node */
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0 && n == sizeof(dl));
  CHECK(flash.n_rx == 1 && rx_of(&flash, SENDER_A) == 1);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EALREADY);
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
  env[12] ^= 1;
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EACCES);
  env[12] ^= 1;
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, pt, 5, out, sizeof(out), &n) == -EBADMSG);

  /* reboot: counters survive, sending resumes above the mark, old downlinks stay refused */
  {
    struct sensei_e2e e2;
    struct sensei_e2e_nv nv = flash;
    uint32_t mark = flash.tx_reserved;

    CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 4) == 0 && sensei_e2e_ready(&e2));
    CHECK(sensei_e2e_key_id(&e2) == sensei_e2e_key_id(&e));
    m = sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env));
    CHECK(m > 0 && sensei_secure_open(&b.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0);
    CHECK(sensei_secure_rx_highest(&b.s, 0) == mark + 1);
    m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, 3, env, sizeof(env));
    CHECK(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);

    /* the same collector key again keeps the counters, the node keeps its key id */
    {
      uint32_t id = sensei_e2e_key_id(&e2), hi = e2.nv.tx_reserved;

      CHECK(sensei_e2e_provision(&e2, b.pub, 65) == 0 && sensei_e2e_key_id(&e2) == id && e2.nv.tx_reserved == hi);
      CHECK(flash.tx_reserved == hi);
    }
    /* another collector key: new key id, but the counters go on (the mark never goes back) */
    {
      struct backend b2;
      uint32_t hi = flash.tx_reserved;

      backend_new(&b2);
      CHECK(sensei_e2e_provision(&e2, b2.pub, 65) == 0 && sensei_e2e_key_id(&e2) != sensei_e2e_key_id(&e));
      CHECK(flash.tx_reserved == hi && rx_of(&flash, SENDER_A) == 3);
      backend_session(&b2, e2.node_pub);
      m = sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env));
      CHECK(sensei_secure_open(&b2.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0);
      CHECK(sensei_secure_rx_highest(&b2.s, 0) > hi);
      /* the first collector can no longer talk to this node */
      CHECK(sensei_secure_open(&b.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == SENSEI_SECURE_ERR_KEY_ID);
    }
  }
}

/* A5: collector A, then B, then A again. No (key, counter) of an uplink is used twice, and no
 * downlink of A's first time is accepted again. */
static void test_key_a_b_a(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend a, b;
  uint8_t pt[16] = {0}, env[80], out[80], dl[8] = {1};
  uint32_t first_a[8], last_a = 0;
  uint8_t old_dl[80];
  size_t n;
  int m, old_len;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 64) == 0);
  backend_new(&a);
  backend_new(&b);

  CHECK(sensei_e2e_provision(&e, a.pub, 65) == 0);
  backend_session(&a, e.node_pub);
  for (int i = 0; i < 5; i++) {
    m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
    CHECK(m > 0 && sensei_secure_open(&a.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0);
  }
  last_a = sensei_secure_rx_highest(&a.s, 0);
  CHECK(last_a == 5);
  old_len = sensei_secure_seal(&a.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), old_dl, sizeof(old_dl));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, old_dl, (size_t)old_len, out, sizeof(out), &n) == 0);

  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);

  CHECK(sensei_e2e_provision(&e, a.pub, 65) == 0);
  for (int i = 0; i < 8; i++) {
    m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
    CHECK(m > 0);
    first_a[i] = (uint32_t)env[6] | ((uint32_t)env[7] << 8) | ((uint32_t)env[8] << 16) | ((uint32_t)env[9] << 24);
    CHECK(first_a[i] > last_a); /* the old counters of A are not used again */
    CHECK(i == 0 || first_a[i] == first_a[i - 1] + 1);
  }
  /* the old downlink of A is still a replay after A comes back */
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, old_dl, (size_t)old_len, out, sizeof(out), &n) == -EALREADY);
  /* ... also after a reboot with what is in flash */
  {
    struct sensei_e2e e2;
    struct sensei_e2e_nv nv = flash;

    CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 64) == 0 && sensei_e2e_ready(&e2));
    CHECK(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, old_dl, (size_t)old_len, out, sizeof(out), &n) ==
          -EALREADY);
    m = sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
    CHECK(m > 0);
    CHECK((uint32_t)(env[6] | env[7] << 8 | env[8] << 16 | (uint32_t)env[9] << 24) > first_a[7]);
  }
}

/* H1 and A5, node side: two senders (GUI and collector) seal with the same counters under one key.
 */
static void test_two_senders(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend gui, col;
  uint8_t out[80], env_g[80], env_c[80], dl[8] = {1};
  size_t n;
  int mg, mc;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&col);
  CHECK(sensei_e2e_provision(&e, col.pub, 65) == 0);
  backend_session_as(&col, e.node_pub, SENDER_A);
  gui = col; /* same keys, same private key: a second host with a copy of the key store */
  backend_session_as(&gui, e.node_pub, SENDER_B);

  mc = sensei_secure_seal(&col.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env_c, sizeof(env_c));
  mg = sensei_secure_seal(&gui.s, SENSEI_SECURE_KIND_SETTINGS, NULL, dl, sizeof(dl), env_g, sizeof(env_g));
  CHECK(mc > 0 && mg > 0);
  /* counter 1 of both senders: both are accepted, each in its own window */
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env_c, (size_t)mc, out, sizeof(out), &n) == 0);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_SETTINGS, env_g, (size_t)mg, out, sizeof(out), &n) == 0);
  CHECK(flash.n_rx == 2 && rx_of(&flash, SENDER_A) == 1 && rx_of(&flash, SENDER_B) == 1);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env_c, (size_t)mc, out, sizeof(out), &n) == -EALREADY);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_SETTINGS, env_g, (size_t)mg, out, sizeof(out), &n) == -EALREADY);
}

/* A36: a message is refused when its counter cannot be stored; it can be sent again later. */
static void test_rx_store_failure(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t out[80], env[80], dl[8] = {1};
  size_t n;
  int m;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  backend_session(&b, e.node_pub);
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
  fail_rx = 1;
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EIO);
  CHECK(e.nv.n_rx == 0 && flash.n_rx == 0);
  fail_rx = 0;
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  CHECK(flash.n_rx == 1);
}

/* The sender table is full: the ninth sender is refused. */
static void test_sender_table_full(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t out[80], env[80], dl[8] = {1};
  size_t n;
  int m;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  for (uint32_t i = 1; i <= SENSEI_SECURE_MAX_SENDERS + 1; i++) {
    backend_session_as(&b, e.node_pub, 0x100 + i);
    m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
    CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) ==
          (i <= SENSEI_SECURE_MAX_SENDERS ? 0 : -ENOMEM));
  }
  CHECK(flash.n_rx == SENSEI_SECURE_MAX_SENDERS);
}

/* D4: clearing the table frees the slots; a sender that was refused is accepted, an old one starts
 * again. */
static void test_clear_senders(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t out[80], env[80], first[80], dl[8] = {1};
  size_t n;
  int m, m_first = 0;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  CHECK(sensei_e2e_senders_used(&e) == 0 && sensei_e2e_senders_max() == SENSEI_SECURE_MAX_SENDERS);
  for (uint32_t i = 1; i <= SENSEI_SECURE_MAX_SENDERS; i++) {
    backend_session_as(&b, e.node_pub, 0x100 + i);
    m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
    CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
    if (i == 1) {
      memcpy(first, env, (size_t)m);
      m_first = m;
    }
  }
  CHECK(sensei_e2e_senders_used(&e) == SENSEI_SECURE_MAX_SENDERS);
  backend_session_as(&b, e.node_pub, 0x200);
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -ENOMEM);

  /* a failed flash write leaves the table as it was */
  fail_rx = 1;
  CHECK(sensei_e2e_clear_senders(&e) == -EIO && sensei_e2e_senders_used(&e) == SENSEI_SECURE_MAX_SENDERS);
  fail_rx = 0;

  CHECK(sensei_e2e_clear_senders(&e) == 0);
  CHECK(sensei_e2e_senders_used(&e) == 0 && flash.n_rx == 0 && sensei_e2e_ready(&e));
  /* the floor is the highest counter of the cleared entries (all had counter 1) */
  CHECK(flash.rx_floor == 1);
  /* recorded downlinks of the cleared senders, and of any sender at the floor, are refused */
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, first, (size_t)m_first, out, sizeof(out), &n) == -EALREADY);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EALREADY);
  CHECK(sensei_e2e_senders_used(&e) == 0);
  /* a new sender above the floor passes */
  backend_session_from(&b, e.node_pub, 0x300, 10);
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  CHECK(sensei_e2e_senders_used(&e) == 1 && flash.n_rx == 1 && rx_of(&flash, 0x300) == 11);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EALREADY);
}

/* F5: the floor covers the replay window of a sender that goes on, survives a restart, only rises
 * and is erased by a reset. A failed write of the floor leaves the table. */
static void test_replay_after_clear(void) {
  struct sensei_e2e e, e2;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t out[80], env[8][80], dl[8] = {1};
  int m[8];
  size_t n;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  backend_session_as(&b, e.node_pub, SENDER_A);
  for (int i = 1; i <= 7; i++) { /* the recorded downlinks, counters 1 to 7 */
    m[i] = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, dl, sizeof(dl), env[i], sizeof(env[i]));
    CHECK(m[i] > 0);
  }
  for (int i = 1; i <= 5; i++) {
    CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env[i], (size_t)m[i], out, sizeof(out), &n) == 0);
  }
  /* the flash refuses the floor: nothing changes */
  fail_floor = 1;
  CHECK(sensei_e2e_clear_senders(&e) == -EIO && sensei_e2e_senders_used(&e) == 1 && flash.rx_floor == 0);
  fail_floor = 0;
  CHECK(sensei_e2e_clear_senders(&e) == 0 && flash.rx_floor == 5 && sensei_e2e_senders_used(&e) == 0);
  /* the same sender goes on: counter 6 and 7 pass, and what it sent before does not, even
   * counters of its old window that it had not been seen in the new table */
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env[7], (size_t)m[7], out, sizeof(out), &n) == 0);
  for (int i = 1; i <= 5; i++) {
    CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env[i], (size_t)m[i], out, sizeof(out), &n) == -EALREADY);
  }
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env[6], (size_t)m[6], out, sizeof(out), &n) ==
        0); /* in the window, above the floor, not seen: it is a message of this sender */
  /* a restart keeps the floor */
  CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &flash, 4) == 0);
  CHECK(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env[3], (size_t)m[3], out, sizeof(out), &n) == -EALREADY);
  /* a second clear only raises it */
  CHECK(sensei_e2e_clear_senders(&e2) == 0 && flash.rx_floor == 7);
  CHECK(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env[6], (size_t)m[6], out, sizeof(out), &n) == -EALREADY);
  CHECK(sensei_e2e_clear_senders(&e2) == 0 && flash.rx_floor == 7);
  /* a reset erases it (with the key pair) */
  CHECK(sensei_e2e_reset(&e2) == 0 && e2.nv.rx_floor == 0 && flash.rx_floor == 0);
}

static void test_reset(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t pub_before[65];

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  memcpy(pub_before, e.node_pub, 65);
  erased = 0;
  CHECK(sensei_e2e_reset(&e) == 0 && erased == 1 && !sensei_e2e_ready(&e) && !flash.have_backend);
  CHECK(flash.have_priv && memcmp(pub_before, e.node_pub, 65) != 0);
}

/* A damaged private key is replaced; the collector must provision again. */
static void test_damaged_key(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  struct backend b;

  memset(&nv, 0, sizeof(nv));
  memset(&flash, 0, sizeof(flash));
  backend_new(&b);
  nv.have_priv = true; /* all zero: not a valid scalar */
  nv.have_backend = true;
  memcpy(nv.backend_pub, b.pub, 65);
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &nv, 4) == 0);
  CHECK(!sensei_e2e_ready(&e) && flash.have_priv && e.node_pub[0] == 4);
}

/* A stored key that cannot be read (sealed record that does not open): no new key pair over it, reset is the way out.
 */
static void test_key_unreadable(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  struct backend b;

  memset(&flash, 0, sizeof(flash));
  memset(&nv, 0, sizeof(nv));
  backend_new(&b);
  nv.key_unreadable = true; /* what the storage sets: have_priv is false, the stored record stays */
  nv.have_backend = true;
  memcpy(nv.backend_pub, b.pub, 65);
  nv.priv[0] = 7; /* must not stay in the struct */
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &nv, 4) == -EACCES);
  CHECK(!flash.have_priv && !sensei_e2e_ready(&e) && !e.nv.have_priv && e.nv.priv[0] == 0); /* nothing stored */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, nv.priv, 4, nv.priv, 0) == -ENOTCONN);
  erased = 0;
  CHECK(sensei_e2e_reset(&e) == 0 && erased == 1 && flash.have_priv && e.nv.have_priv && !e.nv.key_unreadable);
}

/* The new key pair cannot be stored: the start fails and no key stays in RAM. */
static void test_key_store_failure(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  fail_priv = 1;
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == -EIO);
  fail_priv = 0;
  CHECK(!flash.have_priv && !e.nv.have_priv && !sensei_e2e_ready(&e));
  for (size_t i = 0; i < sizeof(e.nv.priv); i++) {
    CHECK(e.nv.priv[i] == 0);
  }
  fail_priv = 1;
  CHECK(sensei_e2e_reset(&e) == -EIO && !e.nv.have_priv);
  fail_priv = 0;
  CHECK(sensei_e2e_reset(&e) == 0 && e.nv.have_priv && flash.have_priv);
}

int main(void) {
  C = sensei_secure_crypto_openssl();
  test_first_start();
  test_provision_and_traffic();
  test_key_a_b_a();
  test_two_senders();
  test_rx_store_failure();
  test_sender_table_full();
  test_clear_senders();
  test_replay_after_clear();
  test_reset();
  test_damaged_key();
  test_key_unreadable();
  test_key_store_failure();
  return CHECK_DONE();
}
