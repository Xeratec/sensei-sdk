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
#include "sensei_ratchet.h"
#include "vectors.h"

static const struct sensei_secure_crypto *C;

/* fake flash. Every write goes through write_gate(): it counts the writes, can fail the nth one (fail_at, once), and
 * keeps a snapshot of the flash after each write (snap_on) for the power-loss replay. */
static struct sensei_e2e_nv flash;
static int erased, fail_tx, fail_rx, fail_floor, fail_priv, fail_erase, fail_rat, fail_backend;
static int write_count, fail_at, tx_writes;
#define MAX_SNAP 2048
static struct sensei_e2e_nv *snaps; /* where write_done() puts them */
static int n_snaps;
static bool snap_on;
static uint32_t t_up, t_unix; /* fake clocks, s */
static uint32_t clock_up(void *u) {
  (void)u;
  return t_up;
}
static uint32_t clock_unix(void *u) {
  (void)u;
  return t_unix;
}

static void snap_before_write(void);

static int write_gate(void) {
  snap_before_write();
  write_count++;
  return fail_at != 0 && write_count == fail_at ? -EIO : 0;
}

static void write_done(void) {
  if (snap_on && n_snaps < MAX_SNAP) {
    snaps[n_snaps++] = flash;
  }
}

#define SENDER_A 0x0A0A0A0Au
#define SENDER_B 0x0B0B0B0Bu

static int f_priv(void *u, const uint8_t *p) {
  (void)u;
  if (fail_priv || write_gate()) {
    return -EIO;
  }
  memcpy(flash.priv, p, sizeof(flash.priv));
  flash.have_priv = true;
  write_done();
  return 0;
}
static int f_backend(void *u, const uint8_t *p) {
  (void)u;
  if (fail_backend || write_gate()) {
    return -EIO;
  }
  memcpy(flash.backend_pub, p, sizeof(flash.backend_pub));
  flash.have_backend = true;
  write_done();
  return 0;
}
static int f_tx(void *u, uint32_t r) {
  (void)u;
  if (fail_tx || write_gate()) {
    return -EIO;
  }
  flash.tx_reserved = r;
  tx_writes++;
  write_done();
  return 0;
}
static int f_rx(void *u, const struct sensei_secure_rx_state *tab, size_t n) {
  (void)u;
  if (fail_rx || write_gate()) {
    return -EIO;
  }
  memcpy(flash.rx, tab, n * sizeof(*tab));
  flash.n_rx = n;
  write_done();
  return 0;
}
/* The record of another key id than the session's is "old": the real backend reads the key id from the record. The
 * fake keeps the key id of the last write in rat_key_id and reports have_rat only if it opens for that session. */
static int f_rat(void *u, uint32_t key_id, const uint8_t *rec, size_t len) {
  (void)u;
  (void)key_id;
  if (fail_rat || write_gate()) {
    return -EIO;
  }
  if (rec == NULL) {
    memset(flash.rat, 0, sizeof(flash.rat));
    flash.have_rat = false;
  } else {
    CHECK(len == SENSEI_E2E_RAT_SIZE);
    memcpy(flash.rat, rec, len);
    flash.have_rat = true;
  }
  flash.rat_unreadable = false;
  write_done();
  return 0;
}
static int f_erase(void *u) {
  (void)u;
  if (fail_erase) {
    return -EIO;
  }
  memset(&flash, 0, sizeof(flash));
  erased++;
  return 0;
}
static int f_floor(void *u, uint32_t f) {
  (void)u;
  if (fail_floor || write_gate()) {
    return -EIO;
  }
  flash.rx_floor = f;
  write_done();
  return 0;
}
static const struct sensei_e2e_ops ops = {.save_priv = f_priv,
                                          .save_backend = f_backend,
                                          .save_tx = f_tx,
                                          .save_rx = f_rx,
                                          .erase = f_erase,
                                          .save_floor = f_floor,
                                          .save_rat = f_rat,
                                          .uptime_s = clock_up,
                                          .unix_s = clock_unix};

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

  /* node to backend: uplink and host data, counters 1 and 2, flash written once per epoch (the block ends at 63) */
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
  CHECK(m == (int)(sizeof(pt) + 26) && flash.tx_reserved == 63);
  CHECK(sensei_secure_open(&b.s, SENSEI_SECURE_KIND_UPLINK, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0);
  CHECK(n == sizeof(pt) && memcmp(out, pt, n) == 0);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_NODE_TO_HOST, pt, 5, env, sizeof(env));
  CHECK(m == 5 + 26 && flash.tx_reserved == 63);
  CHECK(sensei_secure_open(&b.s, 0, NULL, env, (size_t)m, out, sizeof(out), &n, NULL) == 0 &&
        sensei_secure_rx_highest(&b.s, 0) == 2);
  /* a downlink kind cannot be sealed by the node */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_DOWNLINK, pt, 5, env, sizeof(env)) == -EACCES);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, 20) == -ENOSPC);
  /* flash failure: sending stops at the reserved limit, nothing is sent without a stored mark */
  for (int i = 0; i < 61; i++) { /* counters 3 to 63 */
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env)) > 0);
  }
  CHECK(flash.tx_reserved == 63);
  fail_tx = 1;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env)) == -EIO);
  fail_tx = 0;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, 5, env, sizeof(env)) > 0);
  CHECK(flash.tx_reserved > 127 && (flash.tx_reserved & 63u) == 63u); /* the clock stands still: a block of 16 epochs */

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

/* A reset whose erase fails is refused: the error comes back, the session and the key stay, the counters go on. */
static void test_reset_refused_when_erase_fails(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;
  uint8_t pub_before[65], pt[4] = {1, 2, 3, 4}, env[64];
  int m;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0);
  memcpy(pub_before, e.node_pub, 65);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
  CHECK(m > 0);
  erased = 0;
  fail_erase = 1;
  CHECK(sensei_e2e_reset(&e) == -EIO && erased == 0);
  fail_erase = 0;
  CHECK(sensei_e2e_ready(&e) && e.nv.have_priv && e.nv.have_backend && memcmp(pub_before, e.node_pub, 65) == 0);
  CHECK(flash.have_priv && flash.have_backend);                                                 /* nothing was erased */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) == m); /* still talks */
  CHECK(sensei_e2e_reset(&e) == 0 && erased == 1 && !sensei_e2e_ready(&e)); /* and the next try works */
}

/* sensei_secure_priv_valid(): 0 and the order and above are not scalars. */
static void test_priv_valid(void) {
  uint8_t k[32] = {0};
  static const uint8_t order[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17,
                                    0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
  sensei_secure_key_t h = 0;

  CHECK(!sensei_secure_priv_valid(k)); /* 0 */
  k[31] = 1;
  CHECK(sensei_secure_priv_valid(k) && C->ecdh_import(C->ctx, k, 32, &h) == 0);
  C->key_destroy(C->ctx, h);
  memcpy(k, order, 32);
  CHECK(!sensei_secure_priv_valid(k) && C->ecdh_import(C->ctx, k, 32, &h) != 0); /* n */
  k[31] -= 1;
  CHECK(sensei_secure_priv_valid(k) && C->ecdh_import(C->ctx, k, 32, &h) == 0); /* n - 1 */
  C->key_destroy(C->ctx, h);
  k[31] += 2;
  CHECK(!sensei_secure_priv_valid(k)); /* n + 1 */
  memset(k, 0xFF, 32);
  CHECK(!sensei_secure_priv_valid(k));
  k[0] = 0xFE;
  CHECK(sensei_secure_priv_valid(k) == (memcmp(k, order, 32) < 0)); /* below n in the first byte */
  CHECK(sensei_secure_priv_valid(k));
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

/* A key from a sealed record that cannot be turned into a public key is not "damaged": no new key pair, e2e stays off,
 * nothing is stored. Without the flag (plain key) the key is replaced, see above. */
static void test_sealed_key_crypto_error(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  struct backend b;

  memset(&nv, 0, sizeof(nv));
  memset(&flash, 0, sizeof(flash));
  backend_new(&b);
  nv.have_priv = true;
  nv.priv_sealed = true;
  /* nv.priv is all zero: not a valid scalar, the stand-in for a failing import */
  nv.have_backend = true;
  memcpy(nv.backend_pub, b.pub, 65);
  erased = 0;
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &nv, 4) == -EIO);
  CHECK(!flash.have_priv && erased == 0 && !sensei_e2e_ready(&e) && !e.nv.have_priv);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, nv.priv, 4, nv.priv, 0) == -ENOTCONN);
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

/* L4: a reset whose erase failed leaves the key pair in RAM and part of the records in flash: provisioning is refused
 * until a reset succeeds, or it would store a collector key for a key that may be gone. */
static void test_reset_then_provision_is_refused_while_dirty(void) {
  struct sensei_e2e e;
  struct sensei_e2e_nv empty;
  struct backend b;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  write_count = fail_at = 0;
  CHECK(sensei_e2e_init(&e, C, &ops, NULL, &empty, 4) == 0);
  backend_new(&b);
  fail_erase = 1;
  CHECK(sensei_e2e_reset(&e) == -EIO);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == -EBUSY && !flash.have_backend && !e.nv.have_backend);
  fail_erase = 0;
  CHECK(sensei_e2e_reset(&e) == 0);
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0 && flash.have_backend);
}

/* ====================================================================================================================
 * Forward secrecy: the device state machine against a model of the collector
 * ====================================================================================================================
 */

#define RSENDER 0x0C0C0C0Cu
#define MAX_GEN 32

struct head { /* the uplink chain and downlink key of one generation */
  bool have;
  uint32_t up_base;
  uint8_t ck0[32];
  uint8_t k_down[16];
};

/* The collector as the design describes it, without replay windows (every envelope is opened on its own): the static
 * keys, the promoted generation with its root, the keys of every generation so far, and at most one "next". */
struct collector {
  struct sensei_secure_keys keys;
  uint32_t gen;
  uint8_t root[32];
  struct head head[MAX_GEN];
  bool has_next;
  uint32_t n_up_base;
  uint8_t n_e_n[32], n_e_c[32], n_root[32];
  struct head n_head;
  uint32_t down_ctr;
  uint32_t seal_gen; /* generation of the next downlink */
};

static struct collector *g_col; /* the one that the flash snapshots record */

static void coll_init(struct collector *col, const uint8_t *node_pub, const struct backend *b) {
  sensei_secure_key_t own = 0;

  memset(col, 0, sizeof(*col));
  CHECK(C->ecdh_import(C->ctx, b->priv, 32, &own) == 0);
  CHECK(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_BACKEND, node_pub, b->pub, &col->keys) == 0);
  C->key_destroy(C->ctx, own);
  CHECK(sensei_ratchet_root0(C, &col->keys, col->root) == 0);
}

static bool varint(const uint8_t **p, const uint8_t *end, uint32_t *v) {
  *v = 0;
  for (unsigned shift = 0; shift < 35 && *p < end; shift += 7) {
    uint8_t b = *(*p)++;

    *v |= (uint32_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      return true;
    }
  }
  return false;
}

/* Uplink field 14: tag 0x72, length, RekeyRequest{gen = 1, pub = 2, up_base = 3}. */
static bool parse_request(const uint8_t *buf, size_t len, uint32_t *gen, uint8_t pub[32], uint32_t *up_base) {
  const uint8_t *p = buf + 2, *end = buf + len;

  if (len < 2 + 2 + 34 + 2 || buf[0] != 0x72 || buf[1] != len - 2 || len > SENSEI_E2E_REKEY_MAX) {
    return false;
  }
  if (*p++ != 0x08 || !varint(&p, end, gen)) {
    return false;
  }
  if (p + 34 > end || p[0] != 0x12 || p[1] != 32) {
    return false;
  }
  memcpy(pub, p + 2, 32);
  p += 34;
  if (p >= end || *p++ != 0x18 || !varint(&p, end, up_base)) {
    return false;
  }
  return p == end;
}

struct answer {
  uint32_t gen, up_base;
  uint8_t pub[32];
};

/* The responder: an answer for a request of generation cur + 1, the same one again for a repeated request. */
static bool coll_answer(struct collector *col, const uint8_t *req, size_t len, struct answer *a) {
  uint32_t gen, up_base;
  uint8_t e_n[32];
  struct sensei_ratchet_mix mix;

  if (!parse_request(req, len, &gen, e_n, &up_base) || gen != col->gen + 1) {
    return false;
  }
  if (!(col->has_next && col->n_up_base == up_base && memcmp(col->n_e_n, e_n, 32) == 0)) {
    uint8_t priv[32];

    CHECK(C->x25519_keygen(C->ctx, priv, col->n_e_c) == 0);
    CHECK(sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_BACKEND, priv, col->root, col->keys.key_id, gen, up_base, e_n,
                             col->n_e_c, &mix) == 0);
    memset(priv, 0, sizeof(priv));
    col->has_next = true;
    col->n_up_base = up_base;
    memcpy(col->n_e_n, e_n, 32);
    memcpy(col->n_root, mix.root, 32);
    col->n_head.have = true;
    col->n_head.up_base = up_base;
    memcpy(col->n_head.ck0, mix.ck, 32);
    memcpy(col->n_head.k_down, mix.k_down, 16);
  }
  a->gen = gen;
  a->up_base = up_base;
  memcpy(a->pub, col->n_e_c, 32);
  return true;
}

static const struct head *coll_head(const struct collector *col, unsigned bits) {
  for (uint32_t g = col->gen + (col->has_next ? 1u : 0u); g >= 1; g--) {
    if ((g & 3u) == bits) {
      return g == col->gen + 1 ? &col->n_head : (col->head[g].have ? &col->head[g] : NULL);
    }
  }
  return NULL;
}

/* The message key of the epoch of counter, from the chain head. */
static int chain_mk(const struct head *h, uint32_t counter, uint8_t mk[16]) {
  struct sensei_ratchet_chain ch;
  uint32_t ep = sensei_ratchet_epoch(counter);

  if (ep < h->up_base) {
    return -1;
  }
  sensei_ratchet_chain_init(&ch, h->ck0, h->up_base);
  if (sensei_ratchet_chain_key(C, &ch, counter, true, SENSEI_RATCHET_MAX_SKIP, mk) != 0) {
    return -1;
  }
  return 0;
}

static void nonce_of(uint8_t n[12], unsigned dir, uint32_t sender, uint32_t ctr) {
  memset(n, 0, 12);
  n[0] = (uint8_t)dir;
  for (int i = 0; i < 4; i++) {
    n[4 + i] = (uint8_t)(sender >> (8 * i));
    n[8 + i] = (uint8_t)(ctr >> (8 * i));
  }
}

/* Open a node to backend envelope under the keys of the collector: promotes "next" on the first envelope of the new
 * generation, as the design says. Returns the plain length or a negative number; *gen is the generation it was under.
 */
static int coll_open(struct collector *col, const uint8_t *env, size_t len, uint8_t *out, size_t cap, uint32_t *gen_out,
                     uint32_t *ctr_out) {
  struct sensei_secure_info info;
  uint8_t key[16], nonce[12];
  sensei_secure_key_t k = 0;
  size_t hs, n = 0;
  uint32_t gen = 0;
  int rc;

  if (sensei_secure_peek(env, len, &info) != 0 || info.to_node) {
    return -1;
  }
  if (!info.ratchet) {
    memcpy(key, col->keys.k_up, 16);
  } else {
    const struct head *h = coll_head(col, info.gen_bits);

    if (h == NULL || chain_mk(h, info.counter, key) != 0) {
      return -2;
    }
    for (uint32_t g = col->gen + (col->has_next ? 1u : 0u); g >= 1; g--) {
      if ((g & 3u) == info.gen_bits) {
        gen = g;
        break;
      }
    }
  }
  hs = info.has_node_id ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER;
  nonce_of(nonce, 0, 0, info.counter);
  CHECK(C->aead_import(C->ctx, key, &k) == 0);
  rc = C->aead_decrypt(C->ctx, k, nonce, env, hs, env + hs, len - hs, out, cap, &n);
  C->key_destroy(C->ctx, k);
  if (rc != 0) {
    return -3;
  }
  if (gen == col->gen + 1 && col->has_next) { /* promotion */
    col->gen = gen;
    memcpy(col->root, col->n_root, 32);
    col->head[gen] = col->n_head;
    col->has_next = false;
  }
  *gen_out = gen;
  *ctr_out = info.counter;
  return (int)n;
}

static int coll_tx_key(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  const struct collector *col = u;

  (void)counter;
  *gen = col->seal_gen;
  if (col->seal_gen != 0) {
    memcpy(key, col->head[col->seal_gen].k_down, 16);
  }
  return 0;
}

/* A downlink of the collector, in generation col->seal_gen (0: the static key). */
static int coll_seal_down(struct collector *col, uint32_t sender, const uint8_t *pt, size_t len, uint8_t *out,
                          size_t cap) {
  struct sensei_secure s;
  int n;

  CHECK(sensei_secure_init(&s, C, SENSEI_SECURE_ROLE_BACKEND, &col->keys, sender, col->down_ctr, 1, NULL, 0, NULL) ==
        0);
  sensei_secure_set_keyring(&s, &(struct sensei_secure_keyring){.user = col, .tx_key = coll_tx_key});
  n = sensei_secure_seal(&s, SENSEI_SECURE_KIND_DOWNLINK, NULL, pt, len, out, cap);
  col->down_ctr = s.tx_last;
  sensei_secure_close(&s);
  return n;
}

/* ---- the history of what the node sealed: the counter must be unique (alone: the nonce is (direction, sender,
 * counter), so a counter under two generations is a reuse of the nonce under a key that may be the same), and every
 * envelope must open ---- */

struct hist {
  uint32_t gen, ctr;
};
#define MAX_HIST 4096
static struct hist hist[MAX_HIST];
static int n_hist;

static bool hist_unique(int from, int to) {
  for (int i = from; i < to; i++) {
    for (int j = 0; j < i; j++) {
      if (hist[i].ctr == hist[j].ctr) {
        return false;
      }
    }
  }
  return true;
}

/* snapshots for the power-loss replay: flash after w writes, the collector and the history length before write w+1 */
static struct sensei_e2e_nv fl_after[MAX_SNAP];
static struct collector col_before[MAX_SNAP];
static int ev_before[MAX_SNAP];

static void snap_before_write(void) {
  if (snap_on && g_col && write_count < MAX_SNAP - 1) {
    col_before[write_count] = *g_col; /* write_count is the number of writes done: this is write number + 1 */
    ev_before[write_count] = n_hist;
  }
}

static void node_boot(struct sensei_e2e *e) {
  struct sensei_e2e_nv nv = flash;

  CHECK(sensei_e2e_init(e, C, &ops, NULL, &nv, 64) == 0);
}

static void node_reboot(struct sensei_e2e *e) {
  if (e->ready) {
    sensei_secure_close(&e->s);
  }
  node_boot(e);
}

static void setup(struct sensei_e2e *e, struct collector *col, struct backend *b) {
  struct sensei_e2e_nv empty;

  memset(&empty, 0, sizeof(empty));
  memset(&flash, 0, sizeof(flash));
  write_count = fail_at = 0;
  n_hist = 0;
  t_up = 0;
  t_unix = 1700000000u;
  g_col = NULL;
  snap_on = false;
  CHECK(sensei_e2e_init(e, C, &ops, NULL, &empty, 64) == 0);
  backend_new(b);
  CHECK(sensei_e2e_provision(e, b->pub, 65) == 0);
  coll_init(col, e->node_pub, b);
}

/* The answer reaches the node: a downlink of the collector (it carries the answer), then the call of the API. */
static int deliver(struct sensei_e2e *e, struct collector *col, const struct answer *a) {
  uint8_t dl[40] = {0xAA}, env[120], out[120];
  size_t n;
  uint32_t sg = col->seal_gen;
  int m;

  col->seal_gen = col->gen;
  m = coll_seal_down(col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  col->seal_gen = sg;
  if (m <= 0 || sensei_e2e_open(e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) != 0) {
    return -EIO;
  }
  return sensei_e2e_rekey_answer(e, a->gen, a->pub, 32, a->up_base);
}

/* The check at the key hook of the session (L-5): when the hook is asked for a key, the counter is at or below the mark
 * in flash (e2e_tx) and, from generation 1 on, the chain index in the record in flash is at or below the epoch of the
 * counter and the record is the one of the generation in use. The wrapper goes in before every seal of cycle(). */
static int (*chk_orig)(void *, uint32_t, uint32_t *, uint8_t[16]);
static long chk_calls;

static int chk_tx_key(void *u, uint32_t counter, uint32_t *gen, uint8_t key[16]) {
  const struct sensei_e2e *e = u;
  int rc = chk_orig(u, counter, gen, key);

  chk_calls++;
  CHECK(counter <= flash.tx_reserved);
  if (rc == 0 && *gen != 0) {
    uint32_t idx = (uint32_t)flash.rat[68] | ((uint32_t)flash.rat[69] << 8) | ((uint32_t)flash.rat[70] << 16) |
                   ((uint32_t)flash.rat[71] << 24);
    uint32_t fgen = (uint32_t)flash.rat[8] | ((uint32_t)flash.rat[9] << 8) | ((uint32_t)flash.rat[10] << 16) |
                    ((uint32_t)flash.rat[11] << 24);

    CHECK(flash.have_rat && fgen == *gen);
    CHECK(sensei_ratchet_epoch(counter) >= e->r.up.up_base && idx <= sensei_ratchet_epoch(counter) - e->r.up.up_base);
  }
  return rc;
}

static void hook_check_install(struct sensei_e2e *e) {
  if (e->s.ring.tx_key != NULL && e->s.ring.tx_key != chk_tx_key) {
    chk_orig = e->s.ring.tx_key;
    e->s.ring.tx_key = chk_tx_key;
  }
}

/* One uplink: the node asks for a request, appends it, seals; the collector opens it and answers a request. The answer
 * is delivered unless drop. Returns the sealed length (negative: the seal failed), and what happened in *what:
 * bit 0 request sent, bit 1 answer delivered and the node switched. */
static int cycle(struct sensei_e2e *e, struct collector *col, bool drop, unsigned *what) {
  uint8_t pl[32 + SENSEI_E2E_REKEY_MAX], env[160], out[160];
  int rl = sensei_e2e_rekey_due(e, pl + 24, SENSEI_E2E_REKEY_MAX), m;
  uint32_t gen, ctr, node_gen = sensei_e2e_gen(e);
  size_t plen = 24 + (rl > 0 ? (size_t)rl : 0);

  if (what) {
    *what = 0;
  }
  memset(pl, 0x42, 24);
  hook_check_install(e);
  m = sensei_e2e_seal(e, SENSEI_SECURE_KIND_UPLINK, pl, plen, env, sizeof(env));
  if (m < 0) {
    if (rl > 0) {
      sensei_e2e_rekey_unsent(e);
    }
    return m;
  }
  CHECK(coll_open(col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == (int)plen);
  CHECK(memcmp(out, pl, plen) == 0);
  CHECK(gen == node_gen);
  if (n_hist < MAX_HIST) {
    hist[n_hist].gen = node_gen;
    hist[n_hist].ctr = ctr;
    n_hist++;
  }
  if (rl > 0) {
    struct answer a;

    if (what) {
      *what |= 1u;
    }
    if (coll_answer(col, pl + 24, (size_t)rl, &a) && !drop && deliver(e, col, &a) == 0 && what) {
      *what |= 2u;
    }
  }
  return m;
}

static uint32_t env_counter(const uint8_t *env) {
  return (uint32_t)env[6] | ((uint32_t)env[7] << 8) | ((uint32_t)env[8] << 16) | ((uint32_t)env[9] << 24);
}

/* A node that is in generation g (g mixes), with the collector. */
static void to_gen(struct sensei_e2e *e, struct collector *col, uint32_t g) {
  unsigned what;

  while (sensei_e2e_gen(e) < g) {
    if (sensei_e2e_gen(e) != 0) {
      int rc = sensei_e2e_rekey_now(e);

      CHECK(rc == 0 || rc == -EALREADY);
    }
    CHECK(cycle(e, col, false, &what) > 0);
    CHECK((what & 3u) == 3u);
  }
  CHECK(sensei_e2e_gen(e) == g);
}

/* One step of the long runs: time passes, now and then a burst of traffic (the next block starts), a dropped answer,
 * a restart, a mix by command, a mix by the clock. Returns false if the seal failed. */
static bool scen(struct sensei_e2e *e, struct collector *col, int i) {
  unsigned what;
  bool ok;

  t_up += i % 5 == 0 ? 100 : 7;
  if (i == 150) {
    t_up += 7u * 3600u; /* the retry time of a request whose answer was dropped */
  }
  if (i % 37 == 36) {
    (void)sensei_secure_tx_jump(&e->s, e->s.tx_reserved + 1);
  }
  ok = cycle(e, col, i % 11 == 3, &what) > 0;
  if (i == 130) {
    (void)sensei_e2e_rekey_now(e);
  }
  if (i == 260) {
    t_unix += 8u * 86400u;
  }
  if (i % 90 == 89) {
    node_reboot(e);
  }
  return ok;
}

/* ---- tests ---- */

/* Generation 0 to 1 and on: the request, the answer, uplinks and downlinks in every generation, the previous
 * generation, the wrap of the generation bits. */
static void test_fs_full_mix(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], dl[24] = {7}, env[120], out[120], late[120];
  uint32_t gen, ctr;
  struct sensei_e2e_status st;
  struct answer a;
  size_t n;
  int m, late_len, rl;
  unsigned what;

  setup(&e, &col, &b);
  CHECK(sensei_e2e_gen(&e) == 0);
  /* a downlink sealed in generation 0 that will arrive late */
  late_len = coll_seal_down(&col, RSENDER, dl, sizeof(dl), late, sizeof(late));
  CHECK(late_len > 0);

  /* generation 0 after provisioning: the request is due at once, up_base 1 (above everything used) */
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0 && rl <= (int)SENSEI_E2E_REKEY_MAX);
  CHECK(parse_request(req, (size_t)rl, &gen, out, &ctr) && gen == 1 && ctr == 1);
  CHECK(flash.have_rat && flash.rat[0] == 1 && flash.rat[1] == 2 /* pending */);
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == 0); /* sent: not again until the retry time */
  sensei_e2e_status(&e, &st);
  CHECK(st.gen == 0 && st.pending && !st.damaged);
  CHECK(coll_answer(&col, req, (size_t)rl, &a) && a.gen == 1 && a.up_base == 1);

  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, dl, 10, env, sizeof(env)); /* still generation 0 */
  CHECK(m > 0 && !sensei_secure_is_ratchet(env) && env_counter(env) == 1);
  CHECK(deliver(&e, &col, &a) == 0 && sensei_e2e_gen(&e) == 1);
  sensei_e2e_status(&e, &st);
  CHECK(st.gen == 1 && !st.pending && st.prev && st.up_base == 1 && st.rekeys == 1);
  /* the flash holds the new state: one record, generation 1, no pending request */
  CHECK(flash.rat[1] == 1 /* prev */ && flash.rat[8] == 1);

  /* the first uplink of generation 1 is at 64 * up_base, the ratchet format with generation bits 1 */
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, dl, 10, env, sizeof(env));
  CHECK(m > 0 && sensei_secure_is_ratchet(env) && sensei_secure_gen_bits(env) == 1 && env_counter(env) == 64);
  CHECK(sensei_secure_kind(env) == SENSEI_SECURE_KIND_UPLINK);
  CHECK(coll_open(&col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 10 && gen == 1 && ctr == 64);
  CHECK(col.gen == 1 && !col.has_next); /* the collector promoted */
  /* host data (kind 3) is sealed under the same chain */
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_NODE_TO_HOST, dl, 10, env, sizeof(env));
  CHECK(m > 0 && sensei_secure_kind(env) == SENSEI_SECURE_KIND_NODE_TO_HOST && env_counter(env) == 65);
  CHECK(coll_open(&col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 10);

  /* downlinks: the late one of generation 0 (the node still has the previous key), a new one in generation 1 */
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, late, (size_t)late_len, out, sizeof(out), &n) == 0);
  col.seal_gen = 1;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_secure_is_ratchet(env) &&
        sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0 && n == sizeof(dl));
  /* a downlink of the settings kind as well */
  /* the next request drops the previous key: generation 0 downlinks are refused from then on */
  col.seal_gen = 0;
  late_len = coll_seal_down(&col, RSENDER, dl, sizeof(dl), late, sizeof(late));
  CHECK(sensei_e2e_rekey_now(&e) == 0);
  sensei_e2e_status(&e, &st);
  CHECK(st.pending && !st.prev);
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, late, (size_t)late_len, out, sizeof(out), &n) == -EACCES);
  /* the request goes out with the next uplink, the mix to generation 2 */
  CHECK(cycle(&e, &col, false, &what) > 0 && what == 3u && sensei_e2e_gen(&e) == 2);
  CHECK(cycle(&e, &col, false, &what) > 0 && what == 0u && sensei_e2e_gen(&e) == 2);

  /* generation 1 downlinks are accepted in generation 2 (prev_k_down), generation 0 ones are not */
  col.seal_gen = 1;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  col.seal_gen = 2;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_secure_gen_bits(env) == 2 &&
        sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  /* generations 3 to 6: the generation bits wrap (4 -> 0 with the ratchet bit set) */
  to_gen(&e, &col, 6);
  CHECK(col.gen == 6 || col.gen == 5); /* the collector promotes on the first uplink of the new generation */
  CHECK(cycle(&e, &col, false, &what) > 0 && col.gen == 6);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, dl, 10, env, sizeof(env));
  CHECK(sensei_secure_gen_bits(env) == 2 && sensei_secure_is_ratchet(env)); /* 6 mod 4 */
  col.seal_gen = 5;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_secure_gen_bits(env) == 1 &&
        sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
  col.seal_gen = 4;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_secure_gen_bits(env) == 0 && sensei_secure_is_ratchet(env) &&
        sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) ==
            -EACCES); /* too old */
  CHECK(hist_unique(0, n_hist));
}

static bool same_state(const struct sensei_e2e *x, const struct sensei_e2e *y) {
  return memcmp(&x->r, &y->r, sizeof(x->r)) == 0 && x->s.tx_last == y->s.tx_last &&
         x->nv.tx_reserved == y->nv.tx_reserved;
}

/* A repeated answer does nothing; an answer that does not belong to the request is ignored. */
static void test_fs_answers(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], pub[33];
  struct answer a;
  struct sensei_e2e_nv before;
  struct sensei_e2e e0;
  int rl;

  setup(&e, &col, &b);
  /* no request yet: an answer is ignored */
  memset(pub, 9, sizeof(pub));
  CHECK(sensei_e2e_rekey_answer(&e, 1, pub, 32, 1) == -EALREADY && sensei_e2e_gen(&e) == 0);
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0 && coll_answer(&col, req, (size_t)rl, &a));
  before = flash;
  e0 = e;
  /* wrong generation, wrong up_base, wrong length: -EBADMSG, nothing changes (RAM and flash) */
  CHECK(sensei_e2e_rekey_answer(&e, 2, a.pub, 32, a.up_base) == -EBADMSG);
  CHECK(sensei_e2e_rekey_answer(&e, 1, a.pub, 32, a.up_base + 1) == -EBADMSG);
  CHECK(sensei_e2e_rekey_answer(&e, 1, a.pub, 33, a.up_base) == -EBADMSG);
  CHECK(sensei_e2e_rekey_answer(&e, 1, a.pub, 31, a.up_base) == -EBADMSG);
  CHECK(sensei_e2e_rekey_answer(&e, 1, NULL, 32, a.up_base) == -EBADMSG);
  CHECK(sensei_e2e_rekey_answer(&e, 0, a.pub, 32, a.up_base) == -EALREADY); /* not above the generation */
  /* a public key of low order: the shared secret is zero */
  for (size_t i = 0; i < VEC_RAT_LOW_COUNT; i++) {
    int rc = sensei_e2e_rekey_answer(&e, 1, vec_rat_low[i], 32, a.up_base);

    CHECK(rc == -EACCES || rc == -EIO);
  }
  CHECK(same_state(&e, &e0) && memcmp(&before, &flash, sizeof(flash)) == 0 && sensei_e2e_gen(&e) == 0);
  CHECK(e.r.pending);
  /* a wrong but valid-looking key: the switch happens, but the keys differ from the collector's: the node's uplinks do
   * not open. (An answer is not authenticated by itself: it travels in an envelope of the current generation.) The
   * right one: */
  CHECK(sensei_e2e_rekey_answer(&e, a.gen, a.pub, 32, a.up_base) == 0 && sensei_e2e_gen(&e) == 1);
  /* the same answer again: ignored, no change */
  e0 = e;
  before = flash;
  CHECK(sensei_e2e_rekey_answer(&e, a.gen, a.pub, 32, a.up_base) == -EALREADY);
  CHECK(sensei_e2e_rekey_answer(&e, a.gen + 1, a.pub, 32, a.up_base) == -EALREADY);
  CHECK(same_state(&e, &e0) && memcmp(&before, &flash, sizeof(flash)) == 0);
  /* and it opens at the collector */
  {
    uint8_t env[100], out[100];
    uint32_t gen, ctr;
    int m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pub, 12, env, sizeof(env));

    CHECK(m > 0 && coll_open(&col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 12 && gen == 1);
  }
}

/* A write that fails keeps the old state, in RAM and in flash, at every site. */
static void test_fs_failing_writes(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], pl[8] = {1}, env[100];
  struct answer a;
  struct sensei_e2e e0;
  struct sensei_e2e_nv before;
  unsigned what;
  int rl, m;

  setup(&e, &col, &b);
  /* 1. the request cannot be stored: it is not created, nothing is sent */
  fail_rat = 1;
  e0 = e;
  before = flash;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == -EIO);
  CHECK(!e.r.pending && same_state(&e, &e0) && memcmp(&before, &flash, sizeof(flash)) == 0);
  CHECK(sensei_e2e_rekey_now(&e) == -EIO && !e.r.pending);
  fail_rat = 0;
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0 && e.r.pending);

  /* 2. the new state cannot be stored: the node stays in generation 0, pending; the same answer works later */
  CHECK(coll_answer(&col, req, (size_t)rl, &a));
  fail_rat = 1;
  e0 = e;
  before = flash;
  CHECK(sensei_e2e_rekey_answer(&e, a.gen, a.pub, 32, a.up_base) == -EIO);
  CHECK(sensei_e2e_gen(&e) == 0 && e.r.pending && same_state(&e, &e0) && memcmp(&before, &flash, sizeof(flash)) == 0);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  CHECK(m > 0 && !sensei_secure_is_ratchet(env)); /* still talks in generation 0 */
  fail_rat = 0;
  CHECK(sensei_e2e_rekey_answer(&e, a.gen, a.pub, 32, a.up_base) == 0 && sensei_e2e_gen(&e) == 1);

  /* 3. a counter block that cannot be stored: the seal fails, the counter is not used, the node goes on later. The
   * chain key of the block is stored first (a crash in between leaves the old mark: nothing of the block was used). */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0); /* the first block */
  CHECK(sensei_secure_tx_jump(&e.s, e.s.tx_reserved + 1) == 0); /* the start of the next block */
  {
    uint32_t last = e.s.tx_last, res = e.s.tx_reserved;
    uint32_t idx0 = (uint32_t)flash.rat[68] | ((uint32_t)flash.rat[69] << 8);

    fail_rat = 1;
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EIO);
    CHECK(e.s.tx_last == last && e.s.tx_reserved == res && flash.tx_reserved == res);
    CHECK(((uint32_t)flash.rat[68] | ((uint32_t)flash.rat[69] << 8)) == idx0);
    fail_rat = 0;
    fail_tx = 1;
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EIO);
    CHECK(e.s.tx_last == last && e.s.tx_reserved == res && flash.tx_reserved == res);
    CHECK(((uint32_t)flash.rat[68] | ((uint32_t)flash.rat[69] << 8)) > idx0); /* the key went first */
    fail_tx = 0;
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    CHECK(flash.tx_reserved > res);
  }

  /* 4. provisioning: the collector key cannot be stored: nothing changes, the old session talks on */
  {
    struct backend b2;

    backend_new(&b2);
    e0 = e;
    before = flash;
    fail_backend = 1;
    CHECK(sensei_e2e_provision(&e, b2.pub, 65) == -EIO);
    fail_backend = 0;
    CHECK(same_state(&e, &e0) && memcmp(&before, &flash, sizeof(flash)) == 0 && sensei_e2e_gen(&e) == 1);
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    /* ... or the old ratchet record cannot be deleted: the node does not talk until the call is repeated */
    fail_rat = 1;
    CHECK(sensei_e2e_provision(&e, b2.pub, 65) == -EIO);
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EPERM);
    CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, 50, env, sizeof(env), NULL) == -EPERM);
    fail_rat = 0;
    CHECK(sensei_e2e_provision(&e, b2.pub, 65) == 0 && sensei_e2e_gen(&e) == 0 && !flash.have_rat);
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  }

  /* 5. every single write of a longer run, failed once: the run goes on, nothing is used twice, the end is sane */
  {
    int total;

    setup(&e, &col, &b);
    write_count = 0;
    for (int i = 0; i < 420; i++) {
      (void)scen(&e, &col, i);
    }
    total = write_count;
    CHECK(total > 25 && sensei_e2e_gen(&e) >= 3);
    for (int k = 1; k <= total; k++) {
      int fails = 0;

      setup(&e, &col, &b);
      write_count = 0;
      fail_at = k;
      for (int i = 0; i < 420; i++) {
        if (!scen(&e, &col, i)) {
          fails++;
        }
      }
      fail_at = 0;
      CHECK(hist_unique(0, n_hist));
      /* the state that the node holds is the state in flash: a restart gives the same generation and talks */
      {
        struct sensei_e2e e2;
        uint32_t g = sensei_e2e_gen(&e);

        node_boot(&e2);
        CHECK(sensei_e2e_gen(&e2) == g);
        CHECK(cycle(&e2, &col, false, &what) > 0);
        CHECK(hist_unique(0, n_hist));
      }
      CHECK(fails <= 3);
    }
  }
}

/* Power loss: a crash after every write of a run. From the flash as it was then (and the collector as it was just
 * before the next write) the node restarts, no (generation, counter) is used again (against everything it sealed
 * before the crash, the worst case: up to the next write), and it still talks to the collector, also across a mix. */
static void test_fs_power_loss(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  unsigned what;
  int total, ev_final;
  struct collector col_final;
  static struct hist hist_full[MAX_HIST];

  setup(&e, &col, &b);
  g_col = &col;
  snap_on = true;
  n_snaps = 0;
  snaps = fl_after;
  fl_after[n_snaps++] = flash; /* after 0 writes of the run */
  write_count = 0;
  /* provision is already done: the run starts there; it has 3 mixes (the first, rekey_now, the time) and reboots */
  for (int i = 0; i < 420; i++) {
    (void)scen(&e, &col, i);
  }
  total = write_count;
  ev_final = n_hist;
  memcpy(hist_full, hist, sizeof(hist));
  col_final = col;
  snap_on = false;
  g_col = NULL;
  CHECK(total > 25 && n_snaps == total + 1 && sensei_e2e_gen(&e) >= 3);
  CHECK(hist_unique(0, n_hist));

  for (int k = 0; k <= total; k++) {
    struct sensei_e2e e2;
    struct collector c2 = k < total ? col_before[k] : col_final;
    int pre = k < total ? ev_before[k] : ev_final;
    uint32_t g_before;

    memset(&e2, 0, sizeof(e2));
    flash = fl_after[k];
    memcpy(hist, hist_full, sizeof(hist));
    n_hist = pre;
    write_count = 0;
    fail_at = 0;
    node_boot(&e2);
    g_before = sensei_e2e_gen(&e2);
    CHECK(sensei_e2e_ready(&e2));
    for (int i = 0; i < 12; i++) {
      t_up += 100;
      CHECK(cycle(&e2, &c2, false, &what) > 0);
    }
    CHECK(hist_unique(0, n_hist));
    CHECK(sensei_e2e_gen(&e2) >= g_before);
    /* and the receive path works in the generation it is in */
    {
      uint8_t dl[8] = {1}, env[80], out[80];
      size_t n;
      int m;

      c2.seal_gen = c2.gen;
      m = coll_seal_down(&c2, RSENDER, dl, sizeof(dl), env, sizeof(env));
      CHECK(m > 0 && sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == 0);
    }
  }
  snaps = NULL;
}

/* Blocks of whole epochs: half the last block (at least one epoch) if the last persist is a minute ago or more, twice
 * the last block if it is less (streaming), up to SENSEI_E2E_BLOCK_EPOCHS_MAX; one epoch after a start; the mark ends
 * on an epoch boundary, and the chain key of the first epoch of the block is stored with it. */
static void test_fs_blocks(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100], mk_ck[32];
  uint32_t up_base, idx;
  const struct head *hp;

  setup(&e, &col, &b);
  /* generation 0: the first block is [1 .. 63], an epoch boundary (up_base of a later mix is then 1) */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  CHECK(flash.tx_reserved == 63);
  to_gen(&e, &col, 1);
  up_base = e.r.up.up_base;
  CHECK(up_base == 1);
  /* the first seal of generation 1 is at counter 64, less than a minute after the last persist: 2 epochs */
  t_up = 30;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  CHECK(env_counter(env) == 64);
  CHECK((flash.tx_reserved & 63u) == 63u && flash.tx_reserved == 64u * 3u - 1u && e.block_epochs == 2u);
  /* run through blocks with different gaps and look at the written state: a fast persist doubles, a slow one halves */
  {
    uint32_t prev = 2;

    for (int round = 0; round < 6; round++) {
      uint32_t res = flash.tx_reserved, first, nres;
      uint32_t span;

      CHECK(sensei_secure_tx_jump(&e.s, res + 1) == 0); /* the start of the next block */
      t_up += round % 2 ? 100 : 20;                     /* a minute or more since the last persist, or less */
      CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
      first = env_counter(env);
      nres = flash.tx_reserved;
      CHECK(first == res + 1 && nres > res);
      span = (nres - (first | 63u)) / 64u + 1u;
      CHECK((nres & 63u) == 63u && span == (round % 2 ? (prev > 1u ? prev / 2u : 1u) : 2u * prev) &&
            e.block_epochs == span);
      prev = span;
      /* the chain key of the first epoch of the block is in flash: up_idx and up_ck */
      idx = (uint32_t)flash.rat[68] | ((uint32_t)flash.rat[69] << 8) | ((uint32_t)flash.rat[70] << 16);
      CHECK(idx == sensei_ratchet_epoch(first) - up_base);
      hp = col.has_next ? &col.n_head : &col.head[1];
      CHECK(hp->have);
      {
        struct sensei_ratchet_chain ch;

        sensei_ratchet_chain_init(&ch, hp->ck0, hp->up_base);
        CHECK(sensei_ratchet_chain_advance(C, &ch, idx, SENSEI_RATCHET_MAX_SKIP) == 0);
        memcpy(mk_ck, ch.ck, 32);
      }
      CHECK(memcmp(flash.rat + 72, mk_ck, 32) == 0);
    }
  }
  /* streaming: persists less than a minute apart keep doubling, up to the cap, and stay there; later ones halve it */
  {
    uint32_t prev = e.block_epochs;

    for (int round = 0; round < 14; round++) {
      uint32_t res = flash.tx_reserved,
               want = 2u * prev > SENSEI_E2E_BLOCK_EPOCHS_MAX ? SENSEI_E2E_BLOCK_EPOCHS_MAX : 2u * prev;

      CHECK(sensei_secure_tx_jump(&e.s, res + 1) == 0);
      t_up += 2;
      CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
      CHECK(e.block_epochs == want && flash.tx_reserved == ((env_counter(env) | 63u) + (want - 1u) * 64u));
      prev = want;
    }
    CHECK(prev == SENSEI_E2E_BLOCK_EPOCHS_MAX);
    CHECK(sensei_secure_tx_jump(&e.s, flash.tx_reserved + 1) == 0);
    t_up += SENSEI_E2E_PERSIST_FAST_S;
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0 &&
          e.block_epochs == SENSEI_E2E_BLOCK_EPOCHS_MAX / 2u);
    /* a slow sender comes down to one epoch */
    for (int k = 0; k < 11; k++) {
      CHECK(sensei_secure_tx_jump(&e.s, flash.tx_reserved + 1) == 0);
      t_up += SENSEI_E2E_PERSIST_FAST_S;
      CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    }
    CHECK(e.block_epochs == 1u);
  }
  /* a restart resumes above the mark, on an epoch boundary, and the key of that epoch comes out of the stored one */
  {
    struct sensei_e2e e2;
    uint32_t res = flash.tx_reserved, gen, ctr;
    uint8_t out[100];
    int m;

    node_boot(&e2);
    CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    CHECK(env_counter(env) == res + 1 && (env_counter(env) & 63u) == 0);
    m = sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
    CHECK(m > 0 && env_counter(env) == res + 2);
    CHECK(coll_open(&col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 8 && gen == 1);
  }
}

/* An epoch key is not used for longer than epoch_max_s: the next seal jumps to the next epoch. A restart starts a new
 * epoch. */
static void test_fs_epoch_time(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100];
  uint32_t c1, c2;
  struct sensei_e2e_cfg cfg;

  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  c1 = env_counter(env);
  t_up += SENSEI_E2E_EPOCH_MAX_S - 1;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  CHECK(env_counter(env) == c1 + 1); /* the same epoch */
  t_up += 1;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  c2 = env_counter(env);
  CHECK(c2 == (c1 | 63u) + 1 && sensei_ratchet_epoch(c2) == sensei_ratchet_epoch(c1) + 1); /* a new epoch */
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0 &&
        env_counter(env) == c2 + 1); /* and it lasts again */
  /* no limit */
  cfg = e.cfg;
  cfg.epoch_max_s = 0;
  sensei_e2e_configure(&e, &cfg);
  t_up += 10u * SENSEI_E2E_EPOCH_MAX_S;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0 &&
        env_counter(env) == c2 + 2);
  /* a restart: a new epoch, even if it is a minute later */
  {
    struct sensei_e2e e2;

    node_boot(&e2);
    CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    CHECK((env_counter(env) & 63u) == 0 && env_counter(env) > c2 + 2);
  }
  CHECK(hist_unique(0, n_hist));
}

/* When a request is made and sent: gen 0 at once, the period (wall clock, or uptime without it), a long chain,
 * rekey_now; repeated every retry_h; never when it is switched off. */
static void test_fs_triggers(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t r1[SENSEI_E2E_REKEY_MAX], r2[SENSEI_E2E_REKEY_MAX];
  struct sensei_e2e_cfg cfg;
  int n1, n2;

  setup(&e, &col, &b);
  /* off: nothing, also in generation 0 */
  cfg = e.cfg;
  cfg.rekey = false;
  sensei_e2e_configure(&e, &cfg);
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) == 0 && sensei_e2e_rekey_now(&e) == -ENOTSUP && !e.r.pending);
  cfg.rekey = true;
  sensei_e2e_configure(&e, &cfg);
  /* a buffer that is too small: nothing marked as sent, and the request exists */
  CHECK(sensei_e2e_rekey_due(&e, r1, 10) == -ENOSPC && e.r.pending);
  n1 = sensei_e2e_rekey_due(&e, r1, sizeof(r1));
  CHECK(n1 > 0);
  /* the retry time */
  t_up += SENSEI_E2E_REKEY_RETRY_H * 3600u - 1;
  CHECK(sensei_e2e_rekey_due(&e, r2, sizeof(r2)) == 0);
  t_up += 1;
  n2 = sensei_e2e_rekey_due(&e, r2, sizeof(r2));
  CHECK(n2 == n1 && memcmp(r1, r2, (size_t)n1) == 0); /* the same request again */
  CHECK(sensei_e2e_rekey_due(&e, r2, sizeof(r2)) == 0);
  sensei_e2e_rekey_unsent(&e); /* the uplink was not sent */
  CHECK(sensei_e2e_rekey_due(&e, r2, sizeof(r2)) == n1);
  CHECK(sensei_e2e_rekey_now(&e) == -EALREADY); /* pending: it is sent again with the next uplink */
  CHECK(sensei_e2e_rekey_due(&e, r2, sizeof(r2)) == n1);
  /* a restart with a request pending: it goes out at the first uplink */
  {
    struct sensei_e2e e2;

    node_boot(&e2);
    CHECK(e2.r.pending && sensei_e2e_gen(&e2) == 0);
    CHECK(sensei_e2e_rekey_due(&e2, r2, sizeof(r2)) == n1 && memcmp(r1, r2, (size_t)n1) == 0);
  }
  /* the mix; then nothing is due for the period */
  sensei_e2e_rekey_unsent(&e);
  to_gen(&e, &col, 1);
  CHECK(cycle(&e, &col, false, NULL) > 0 && !e.r.pending);
  t_up += 3600;
  t_unix += 6u * 86400u;
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) == 0 && !e.r.pending);
  t_unix += 86400u; /* 7 days since the start of the generation (the clock may have run during the mix) */
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) > 0 && e.r.pending);
  sensei_e2e_rekey_unsent(&e);
  CHECK(cycle(&e, &col, false, NULL) > 0 && sensei_e2e_gen(&e) == 2);
  /* without a wall clock: the period is counted in uptime from the start of the generation */
  t_unix = 0;
  {
    struct sensei_e2e e2;

    node_boot(&e2); /* the start of this boot is the start of the count */
    CHECK(sensei_e2e_gen(&e2) == 2 && sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) == 0);
    t_up += 7u * 86400u - 1;
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) == 0);
    t_up += 1;
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) > 0);
  }
  /* the clock appears later: the start of the generation is taken then */
  t_unix = 1800000000u;
  {
    struct sensei_e2e e2;

    node_boot(&e2);
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) > 0); /* it was pending in flash */
    CHECK(e2.r.pending);
  }
  /* a clock that is behind the start of the generation (it was wrong then): the uptime counts */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  t_unix = 1600000000u;
  {
    struct sensei_e2e e2;

    node_boot(&e2);
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) == 0);
    t_up += 7u * 86400u - 1;
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) == 0);
    t_up += 1;
    CHECK(sensei_e2e_rekey_due(&e2, r1, sizeof(r1)) > 0);
  }
  /* a chain of 4096 epochs: a mix, whatever the time */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  CHECK(cycle(&e, &col, false, NULL) > 0);
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) == 0);
  CHECK(sensei_secure_tx_jump(&e.s, 64u * (e.r.up.up_base + SENSEI_E2E_REKEY_EPOCHS - 1) + 1) == 0);
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) == 0); /* 4095 epochs */
  CHECK(sensei_secure_tx_jump(&e.s, 64u * (e.r.up.up_base + SENSEI_E2E_REKEY_EPOCHS) + 1) == 0);
  CHECK(sensei_e2e_rekey_due(&e, r1, sizeof(r1)) > 0); /* 4096 */
  /* the new up_base is above everything that was sent */
  CHECK(e.r.pend_up_base > sensei_ratchet_epoch(e.s.tx_last));
}

/* Provisioning again: back to generation 0, the counters go on, a mix is requested at once. */
static void test_fs_reprovision(void) {
  struct sensei_e2e e;
  struct collector col, col2;
  struct backend b, b2;
  uint8_t pl[8] = {1}, env[100], out[100], req[SENSEI_E2E_REKEY_MAX], dl[8] = {1};
  uint32_t last_ctr, gen, ctr;
  size_t n;
  int m, rl;

  setup(&e, &col, &b);
  to_gen(&e, &col, 2);
  CHECK(cycle(&e, &col, false, NULL) > 0);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  last_ctr = env_counter(env);
  CHECK(m > 0 && flash.have_rat);
  /* the same collector key: the static keys are the same, the ratchet is gone */
  CHECK(sensei_e2e_provision(&e, b.pub, 65) == 0 && sensei_e2e_gen(&e) == 0 && !flash.have_rat && !e.r.pending);
  coll_init(&col2, e.node_pub, &b);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  CHECK(m > 0 && !sensei_secure_is_ratchet(env) && env_counter(env) > last_ctr);
  CHECK(coll_open(&col2, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 8 && gen == 0);
  /* a downlink of generation 2 is refused now */
  col.seal_gen = 2;
  m = coll_seal_down(&col, RSENDER, dl, sizeof(dl), env, sizeof(env));
  CHECK(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n) == -EACCES);
  /* a request at once, above everything that was sent */
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0 && e.r.pend_up_base > sensei_ratchet_epoch(e.s.tx_last));
  /* another collector key: new static keys, the counters still go on */
  backend_new(&b2);
  CHECK(sensei_e2e_provision(&e, b2.pub, 65) == 0 && sensei_e2e_gen(&e) == 0 && !e.r.pending && !flash.have_rat);
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  CHECK(m > 0 && env_counter(env) > last_ctr);
  CHECK(hist_unique(0, n_hist));
}

/* The mark is far ahead of the counter (a block of 8 epochs) when the request is made: up_base is above the mark, the
 * first counter of the new generation is above the mark too, and a restart right after the switch (the flash holds the
 * new state and the old mark) puts the node at that counter, not below it (the chain cannot serve an epoch before
 * up_base). */
static void test_fs_mark_ahead(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100], req[SENSEI_E2E_REKEY_MAX], out[100];
  struct answer a;
  uint32_t res, gen, ctr;
  int rl, m;

  setup(&e, &col, &b);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0); /* [1 .. 63] */
  CHECK(sensei_secure_tx_jump(&e.s, 64) == 0);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0); /* 2 epochs, t = 0 */
  CHECK(sensei_secure_tx_jump(&e.s, flash.tx_reserved + 1) == 0);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0); /* 4 epochs */
  CHECK(sensei_secure_tx_jump(&e.s, flash.tx_reserved + 1) == 0);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0); /* 8 epochs */
  res = flash.tx_reserved;
  CHECK(res == 64 * 15 - 1); /* [64 .. 191] [192 .. 447] [448 .. 959] */
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0 && e.r.pend_up_base == sensei_ratchet_epoch(res) + 1);
  CHECK(coll_answer(&col, req, (size_t)rl, &a) && sensei_e2e_rekey_answer(&e, a.gen, a.pub, 32, a.up_base) == 0);
  CHECK(flash.tx_reserved == res && e.s.tx_last + 1 == 64u * a.up_base);
  /* restart: nothing was sealed since the switch */
  node_boot(&e2);
  m = sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  CHECK(m > 0 && env_counter(env) == 64u * a.up_base);
  CHECK(coll_open(&col, env, (size_t)m, out, sizeof(out), &gen, &ctr) == 8 && gen == 1);
  /* without the restart too */
  m = sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env));
  CHECK(m > 0 && env_counter(env) == 64u * a.up_base);
}

/* A ratchet record that is lost, of another key, or not valid. */
static void test_fs_damaged(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100], out[100];
  struct sensei_e2e_status st;
  struct sensei_e2e_nv saved;
  size_t n;

  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  saved = flash;
  CHECK(saved.have_rat);

  /* the record is for another key id: it is old and ignored, the node is in generation 0 (the keys are new) */
  flash = saved;
  flash.rat[4] ^= 1;
  node_boot(&e2);
  CHECK(sensei_e2e_gen(&e2) == 0 && !e2.damaged &&
        sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) > 0);
  /* the record is there but does not open (the storage says so): refuse to seal and to open, counted */
  flash = saved;
  flash.have_rat = false;
  flash.rat_unreadable = true;
  node_boot(&e2);
  CHECK(sensei_e2e_ready(&e2) && e2.damaged);
  CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) == -EPERM);
  CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_NODE_TO_HOST, pl, 8, env, 100) == -EPERM);
  CHECK(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env, 60, out, sizeof(out), &n) == -EPERM);
  CHECK(sensei_e2e_rekey_due(&e2, out, sizeof(out)) == -EPERM && sensei_e2e_rekey_now(&e2) == -EPERM);
  sensei_e2e_status(&e2, &st);
  CHECK(st.damaged && st.refused == 3);
  /* a restart does not clear it; provisioning does */
  node_boot(&e2);
  CHECK(e2.damaged);
  CHECK(sensei_e2e_provision(&e2, b.pub, 65) == 0 && !e2.damaged && sensei_e2e_gen(&e2) == 0);
  CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) > 0);
  /* a record that opened but is not valid: version, flags, fields */
  for (int i = 0; i < 6; i++) {
    flash = saved;
    switch (i) {
    case 0:
      flash.rat[0] = 2;
      break;
    case 1:
      flash.rat[1] |= 0x80;
      break;
    case 2: /* a pending request with up_base 0 */
      flash.rat[1] |= 2;
      memset(flash.rat + 120, 0, 4);
      break;
    case 3: /* generation 1 with up_base 0 */
      flash.rat[64] = flash.rat[65] = flash.rat[66] = flash.rat[67] = 0;
      break;
    case 4: /* generation 0 without a pending request */
      flash.rat[8] = 0;
      flash.rat[1] = 0;
      break;
    default: /* the chain index out of range */
      flash.rat[71] = 0x7F;
      break;
    }
    node_boot(&e2);
    CHECK(e2.damaged && sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) == -EPERM);
  }
  /* a state of generation 1 that this build cannot use (no X25519): refused, not run as generation 0 */
  {
    struct sensei_secure_crypto c2 = *C;
    struct sensei_e2e_nv nv = saved;

    c2.x25519 = NULL;
    flash = saved;
    CHECK(sensei_e2e_init(&e2, &c2, &ops, NULL, &nv, 64) == 0 && e2.damaged);
    CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) == -EPERM);
    /* without X25519 a node in generation 0 stays there and never asks */
    flash = saved;
    flash.have_rat = false;
    nv = flash;
    CHECK(sensei_e2e_init(&e2, &c2, &ops, NULL, &nv, 64) == 0 && !e2.damaged);
    CHECK(sensei_e2e_rekey_due(&e2, out, sizeof(out)) == 0 && sensei_e2e_rekey_now(&e2) == -ENOTSUP);
    CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, 8, env, 100) > 0);
  }
}

/* The Python implementation as the collector: its transcript (vector 6) replayed on the device. The device makes the
 * bytes that the Python side made (same keys, same ephemeral key through a fixed key generator), and accepts and
 * refuses what the Python side says. */
static int fixed_keygen(void *ctx, uint8_t priv[32], uint8_t pub[32]) {
  (void)ctx;
  memcpy(priv, vec_rat_mix[0].e_n_priv, 32);
  memcpy(pub, vec_rat_mix[0].e_n_pub, 32);
  return 0;
}

static void test_fs_transcript(void) {
  struct sensei_secure_crypto wc = *C;
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  uint8_t out[300], pt[300], req[SENSEI_E2E_REKEY_MAX];
  int due_done = 0, switched = 0;
  int base = check_failures;

  wc.x25519_keygen = fixed_keygen;
  memset(&nv, 0, sizeof(nv));
  memset(&flash, 0, sizeof(flash));
  write_count = fail_at = 0;
  t_up = 0;
  t_unix = 1700000000u;
  nv.have_priv = true;
  memcpy(nv.priv, vec_sec_node_priv, 32);
  nv.have_backend = true;
  memcpy(nv.backend_pub, vec_sec_backend_pub, 65);
  CHECK(sensei_e2e_init(&e, &wc, &ops, NULL, &nv, 64) == 0 && sensei_e2e_key_id(&e) == VEC_SEC_KEY_ID);
  for (int i = 0; i < VEC_RAT_STEPS_COUNT; i++) {
    const vec_rat_step_t *st = &vec_rat_steps[i];
    const vec_rat_step_t *ref = &vec_rat_steps[st->ref];
    size_t n = 0;
    int r;

    switch (st->op) {
    case VEC_RAT_OP_SEAL_N:
      if (!due_done) { /* the request goes with the second uplink in the transcript; the key is the same */
        CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0 && e.r.pend_up_base == VEC_RAT_UP_BASE_T);
        due_done = 1;
      }
      if (st->counter > e.s.tx_last + 1) { /* the transcript's reboots and epoch jumps */
        CHECK(sensei_secure_tx_jump(&e.s, st->counter) == 0);
      }
      r = sensei_e2e_seal(&e, (enum sensei_secure_kind)st->kind, st->plain, st->plain_len, out, sizeof(out));
      CHECK(r == (int)st->sealed_len && r > 0 && memcmp(out, st->sealed, st->sealed_len) == 0);
      break;
    case VEC_RAT_OP_SWITCH: /* the answer of the collector: its ephemeral key and up_base */
      CHECK(sensei_e2e_rekey_answer(&e, 1, vec_rat_mix[0].e_c_pub, 32, VEC_RAT_UP_BASE_T) == 0 && !switched);
      switched = 1;
      break;
    case VEC_RAT_OP_OPEN_N:
      r = sensei_e2e_open(&e, 0, ref->sealed, ref->sealed_len, pt, sizeof(pt), &n);
      CHECK(r == (st->expect == 0 ? 0 : sensei_e2e_errno(st->expect)));
      if (r == 0) {
        CHECK(n == ref->plain_len && memcmp(pt, ref->plain, n) == 0);
      }
      break;
    case VEC_RAT_OP_DROP_PREV: /* the next request drops the previous generation's key */
      CHECK(sensei_e2e_rekey_now(&e) == 0 && !e.r.prev);
      break;
    default: /* the collector opens (the Python side made these, test_ratchet checks them) */
      break;
    }
    if (check_failures != base) {
      printf("device transcript step %d failed\n", i);
      break;
    }
  }
}

/* L-2: flash wear under fast streaming: 100 000 messages at 50 a second (2000 s) write the counter mark about once a
 * minute at most once the block has grown; blocks of at most 16 epochs (1024 counters) would write it 98 times. */
static void test_fs_wear(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100];

  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  tx_writes = 0;
  for (int i = 0; i < 100000; i++) {
    if (i % 50 == 49) {
      t_up += 1;
    }
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  }
  printf("wear: %d counter writes for 100000 messages in 2000 s\n", tx_writes);
  CHECK(tx_writes <= 2000 / 60 + 15 && tx_writes < 98 / 2);
  /* a restart goes on above the mark */
  {
    struct sensei_e2e e2;
    uint32_t res = flash.tx_reserved;

    node_boot(&e2);
    CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
    CHECK(env_counter(env) > e.s.tx_last && env_counter(env) == res + 1);
  }
}

static uint32_t rec_age_h(void) { return (uint32_t)flash.rat[2] | ((uint32_t)flash.rat[3] << 8); }

/* M-1: the record carries the age of the generation (hours, rounded up, written with every write of the record); a
 * restart takes it back, so a node that restarts more often than the period and has no wall clock still mixes; a clock
 * that appears after the switch back-dates the start of the generation by the age. */
static void test_fs_age(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100], req[SENSEI_E2E_REKEY_MAX];
  int boots = 0;

  /* the age in the record: 0 at the switch, then every write has it */
  setup(&e, &col, &b);
  t_unix = 0;
  to_gen(&e, &col, 1);
  CHECK(rec_age_h() == 0);
  t_up = 5u * 3600u + 10u;
  CHECK(sensei_e2e_rekey_now(&e) == 0);
  CHECK(rec_age_h() == 6); /* rounded up */
  t_up = 6u * 3600u;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  node_boot(&e2); /* a restart: the generation started 6 h before (the record said 6 h) */
  CHECK(e2.r.gen_start_up == t_up - 6u * 3600u);
  t_up += 100;
  CHECK(sensei_e2e_rekey_now(&e2) == -EALREADY);
  /* the age is capped at 65535 h and a stored age of 0 (a record of an earlier version) loads as a new generation */
  flash.rat[2] = 0xFF;
  flash.rat[3] = 0xFF;
  node_boot(&e2);
  CHECK(e2.r.gen_start_up == t_up - 0xFFFFu * 3600u);
  flash.rat[2] = flash.rat[3] = 0;
  node_boot(&e2);
  CHECK(e2.r.gen_start_up == t_up);

  /* no wall clock, a restart every 6 days: the mix still comes (the old code counted the uptime of one boot: never) */
  setup(&e, &col, &b);
  t_unix = 0;
  to_gen(&e, &col, 1);
  CHECK(cycle(&e, &col, false, NULL) > 0);
  for (int boot = 0; boot < 3; boot++) {
    int rl;

    t_up = 0; /* a start: the uptime begins again */
    node_boot(&e2);
    boots++;
    if (boot == 1) { /* 6 days of the first boot are in the record, the second boot is a day old: 7 days */
      t_up = 86400u - 1u;
      CHECK(sensei_e2e_rekey_due(&e2, req, sizeof(req)) == 0);
      t_up = 86400u;
      rl = sensei_e2e_rekey_due(&e2, req, sizeof(req));
      CHECK(rl > 0 && e2.r.pending);
      break;
    }
    for (int day = 1; day <= 6; day++) {
      t_up = (uint32_t)day * 86400u; /* the epoch of a day is over: a new epoch, so the record is written again */
      CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
      CHECK(rec_age_h() == (uint32_t)day * 24u + (uint32_t)boot * 144u);
      CHECK(sensei_e2e_rekey_due(&e2, req, sizeof(req)) == 0);
    }
  }
  CHECK(boots == 2);

  /* the clock appears after the switch: the start of the generation is back-dated by the age, so the period counts
   * from the switch and not from the moment the clock came */
  setup(&e, &col, &b);
  t_unix = 0;
  to_gen(&e, &col, 1);
  CHECK(e.r.gen_start_unix == 0);
  t_up = 3u * 86400u;
  t_unix = 1800000000u;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == 0);
  CHECK(e.r.gen_start_unix == 1800000000u - 3u * 86400u);
  t_up += 4u * 86400u - 1u;
  t_unix += 4u * 86400u - 1u;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == 0);
  t_up += 1;
  t_unix += 1;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0);
  /* a clock that is smaller than the age: the start is 1, not a wrap */
  setup(&e, &col, &b);
  t_unix = 0;
  to_gen(&e, &col, 1);
  t_up = 3u * 86400u;
  t_unix = 100;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == 0 && e.r.gen_start_unix == 1);
}

/* M-2: init wipes the caller's copy of the secrets; the node still works, and the caller's flags stay. */
static void test_fs_init_wipes(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  struct sensei_e2e_nv nv;
  uint8_t zero[sizeof(nv.rat)] = {0}, pl[8] = {1}, env[100];
  uint8_t priv[32];

  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  memcpy(priv, flash.priv, 32);
  nv = flash;
  CHECK(nv.have_rat && nv.rat[0] == 1 && memcmp(nv.priv, priv, 32) == 0);
  CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 64) == 0);
  CHECK(memcmp(nv.rat, zero, sizeof(nv.rat)) == 0 && !nv.have_rat);
  CHECK(memcmp(nv.priv, zero, 32) == 0 && nv.have_priv && nv.have_backend);
  CHECK(sensei_e2e_gen(&e2) == 1 &&
        sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  CHECK(memcmp(e2.nv.rat, zero, sizeof(nv.rat)) == 0 && memcmp(e2.nv.priv, priv, 32) == 0);
  /* also when the init fails (a key that cannot be used) */
  nv = flash;
  nv.key_unreadable = true;
  CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 64) == -EACCES);
  CHECK(memcmp(nv.rat, zero, sizeof(nv.rat)) == 0 && memcmp(nv.priv, zero, 32) == 0);
  /* and when the record is unusable */
  nv = flash;
  nv.rat[0] = 9;
  CHECK(sensei_e2e_init(&e2, C, &ops, NULL, &nv, 64) == 0 && e2.damaged);
  CHECK(memcmp(nv.rat, zero, sizeof(nv.rat)) == 0 && memcmp(nv.priv, zero, 32) == 0);
}

/* L-3: a clock that is held back or stopped (network time, GNSS, USB host) does not postpone the mix: the uptime plus
 * the stored age says "due" on its own. */
static void test_fs_clock_attack(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100], req[SENSEI_E2E_REKEY_MAX];

  /* the clock stands still */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  t_up += 7u * 86400u - 1u;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) == 0);
  t_up += 1;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0); /* t_unix did not move */
  /* the clock runs backwards, after a restart that keeps 3 days of age in the record */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  CHECK(cycle(&e, &col, false, NULL) > 0);
  for (int day = 1; day <= 3; day++) {
    t_up = (uint32_t)day * 86400u; /* a new epoch every day: the record is written with the age */
    CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  }
  CHECK(rec_age_h() == 72u);
  t_up = 0;
  t_unix -= 100u * 86400u;
  node_boot(&e2);
  t_up = 4u * 86400u - 1u;
  t_unix -= 1000u;
  CHECK(sensei_e2e_rekey_due(&e2, req, sizeof(req)) == 0);
  t_up += 1;
  CHECK(sensei_e2e_rekey_due(&e2, req, sizeof(req)) > 0);
  /* a clock that runs fast still brings the mix early (the node was off, or the clock is right) */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  t_up += 100;
  t_unix += 7u * 86400u;
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0);
}

/* L-4: a request whose answer does not come while the node streams is replaced by a new one, not repeated: the answer
 * walk stays short and an old answer is refused. */
static void test_fs_stale_request(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], req2[SENSEI_E2E_REKEY_MAX];
  struct answer a_old, a_new;
  uint32_t u1;
  int rl, rl2;

  CHECK(SENSEI_E2E_REKEY_STALE_EPOCHS == 32768u); /* the walk of an answer stays at about 1 to 2 s */
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  CHECK(cycle(&e, &col, false, NULL) > 0); /* the collector is in generation 1 too */
  CHECK(sensei_e2e_rekey_now(&e) == 0);
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req));
  CHECK(rl > 0);
  u1 = e.r.pend_up_base;
  CHECK(coll_answer(&col, req, (size_t)rl, &a_old));
  /* 4096 epochs since the request: the same request is still repeated after the retry time, and answered */
  CHECK(sensei_secure_tx_jump(&e.s, 64u * (u1 + SENSEI_E2E_REKEY_STALE_EPOCHS) + 1) == 0);
  t_up += SENSEI_E2E_REKEY_RETRY_H * 3600u;
  CHECK(sensei_e2e_rekey_due(&e, req2, sizeof(req2)) == rl && e.r.pend_up_base == u1);
  /* one more: the answer is refused (no long walk), and the next uplink carries a new request at once */
  CHECK(sensei_secure_tx_jump(&e.s, 64u * (u1 + SENSEI_E2E_REKEY_STALE_EPOCHS + 1) + 1) == 0);
  CHECK(deliver(&e, &col, &a_old) == -ETIMEDOUT && e.r.pending && sensei_e2e_gen(&e) == 1);
  rl2 = sensei_e2e_rekey_due(&e, req2, sizeof(req2));
  CHECK(rl2 > 0 && e.r.pend_up_base > u1 && e.r.pend_up_base > sensei_ratchet_epoch(e.s.tx_last));
  /* a stale request is replaced before the retry time too, once */
  CHECK(sensei_e2e_rekey_due(&e, req2, sizeof(req2)) == 0);
  CHECK(sensei_secure_tx_jump(&e.s, 64u * (e.r.pend_up_base + SENSEI_E2E_REKEY_STALE_EPOCHS + 1) + 1) == 0);
  u1 = e.r.pend_up_base;
  rl2 = sensei_e2e_rekey_due(&e, req2, sizeof(req2));
  CHECK(rl2 > 0 && e.r.pend_up_base > u1);
  CHECK(coll_answer(&col, req2, (size_t)rl2, &a_new));
  /* the answer of the first request is refused, the new one works and starts at the new epoch */
  CHECK(deliver(&e, &col, &a_old) == -EBADMSG && sensei_e2e_gen(&e) == 1);
  CHECK(deliver(&e, &col, &a_new) == 0 && sensei_e2e_gen(&e) == 2);
  CHECK(e.r.up.idx <= 1);
  CHECK(cycle(&e, &col, false, NULL) > 0);
}

/* L-1: a build without the ratchet drops a generation 0 record that only holds a pending request. */
static void test_fs_no_ratchet_pending(void) {
  struct sensei_e2e e, e2;
  struct collector col;
  struct backend b;
  struct sensei_secure_crypto c2 = *C;
  struct sensei_e2e_nv nv;
  uint8_t pl[8] = {1}, env[100], req[SENSEI_E2E_REKEY_MAX];

  setup(&e, &col, &b);
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0 && e.r.pending && flash.have_rat);
  c2.x25519 = NULL;
  nv = flash;
  CHECK(sensei_e2e_init(&e2, &c2, &ops, NULL, &nv, 64) == 0);
  CHECK(!e2.damaged && sensei_e2e_gen(&e2) == 0 && !e2.r.pending);
  CHECK(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  CHECK(!sensei_secure_is_ratchet(env) && sensei_e2e_rekey_due(&e2, req, sizeof(req)) == 0);
  /* a build with the ratchet takes the same record as before */
  node_boot(&e2);
  CHECK(!e2.damaged && e2.r.pending && sensei_e2e_gen(&e2) == 0);
  /* a record of generation 1 is not dropped: damaged */
  sensei_e2e_rekey_unsent(&e);
  to_gen(&e, &col, 1);
  nv = flash;
  CHECK(sensei_e2e_init(&e2, &c2, &ops, NULL, &nv, 64) == 0 && e2.damaged);
  /* provisioning again clears the old pending record in the build without the ratchet too */
  setup(&e, &col, &b);
  CHECK(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0 && flash.have_rat);
  nv = flash;
  CHECK(sensei_e2e_init(&e2, &c2, &ops, NULL, &nv, 64) == 0 && !e2.damaged);
  CHECK(sensei_e2e_provision(&e2, b.pub, 65) == 0 && !flash.have_rat && !e2.damaged);
}

/* I-4: a failing key hook is its own error, and the checks at the hook hold the line. */
static void test_fs_tx_key_error(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint8_t pl[8] = {1}, env[100];
  uint32_t last, res, fidx;

  CHECK(sensei_e2e_errno(SENSEI_SECURE_ERR_TXKEY) == -EPROTO);
  setup(&e, &col, &b);
  to_gen(&e, &col, 1);
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0);
  last = e.s.tx_last;
  /* a counter above the stored mark never gets a key */
  res = e.nv.tx_reserved;
  e.nv.tx_reserved = last;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EPROTO);
  CHECK(e.s.tx_last == last);
  e.nv.tx_reserved = res;
  /* a chain key in flash that is ahead of the epoch in use never gets a key either */
  fidx = e.flash_idx;
  e.flash_idx = fidx + 1000;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EPROTO);
  e.flash_idx_valid = false;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) == -EPROTO);
  e.flash_idx = fidx;
  e.flash_idx_valid = true;
  CHECK(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pl, sizeof(pl), env, sizeof(env)) > 0 &&
        env_counter(env) == last + 1);
}

/* L-5: the design's property through sensei_e2e and the collector model: 100 000 uplinks, 20 mixes (by the request of
 * the node, some answers dropped), restarts, bursts and pauses. Every envelope opens at the collector; the counter is
 * above every counter before it (so no (key, nonce) pair twice); the check at the key hook runs for every seal
 * (chk_tx_key); the flash is written about once a minute while streaming. */
static void test_fs_property(void) {
  struct sensei_e2e e;
  struct collector col;
  struct backend b;
  uint32_t top = 0, gen_seen = 0, first_mix_msg = 0;
  long msgs = 0, writes0;
  int mixes = 0;
  unsigned what;

  setup(&e, &col, &b);
  hook_check_install(&e);
  chk_calls = 0;
  writes0 = write_count;
  for (long i = 0; i < 100000; i++) {
    int m;

    if (i % 20 == 19) {
      t_up += 1; /* 20 messages a second */
    }
    if (i % 7000 == 6999) {
      t_up += 300; /* a pause: the next block is half the size */
    }
    if (i % 9000 == 8999) {
      node_reboot(&e);
      hook_check_install(&e);
    }
    if (i % 5000 == 2500 && sensei_e2e_gen(&e) != 0 && mixes < 20) {
      int rc = sensei_e2e_rekey_now(&e);

      CHECK(rc == 0 || rc == -EALREADY);
    }
    m = cycle(&e, &col, i % 13 == 5, &what); /* some answers are dropped; the request is sent again after retry_h */
    CHECK(m > 0);
    if (m > 0) {
      uint32_t c = hist[n_hist - 1].ctr;

      CHECK(c > top);
      top = c;
      msgs++;
    }
    if (n_hist == MAX_HIST) {
      n_hist = 0; /* only the last counter is needed: it is checked against top */
    }
    if (what & 2u) {
      mixes++;
      if (!first_mix_msg) {
        first_mix_msg = (uint32_t)i;
      }
    }
    if (sensei_e2e_gen(&e) < gen_seen) {
      CHECK(0);
    }
    gen_seen = sensei_e2e_gen(&e);
    if (what & 1u && i % 13 == 5) {
      t_up += SENSEI_E2E_REKEY_RETRY_H * 3600u; /* the answer was dropped: the request is sent again */
    }
  }
  printf("property: %ld messages, %d mixes, generation %u, %ld key hook calls, %ld flash writes\n", msgs, mixes,
         sensei_e2e_gen(&e), chk_calls, (long)(write_count - writes0));
  CHECK(msgs == 100000 && mixes >= 20 && sensei_e2e_gen(&e) >= 20);
  CHECK(chk_calls >= msgs);
  /* the wear of the counters is checked in test_fs_wear; here the writes of the mixes, answers, restarts add up */
  CHECK(write_count - writes0 < 400);
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
  test_reset_refused_when_erase_fails();
  test_priv_valid();
  test_damaged_key();
  test_sealed_key_crypto_error();
  test_key_unreadable();
  test_key_store_failure();
  test_reset_then_provision_is_refused_while_dirty();
  test_fs_full_mix();
  test_fs_answers();
  test_fs_failing_writes();
  test_fs_blocks();
  test_fs_epoch_time();
  test_fs_triggers();
  test_fs_mark_ahead();
  test_fs_reprovision();
  test_fs_damaged();
  test_fs_transcript();
  test_fs_wear();
  test_fs_age();
  test_fs_clock_attack();
  test_fs_stale_request();
  test_fs_init_wipes();
  test_fs_no_ratchet_pending();
  test_fs_tx_key_error();
  test_fs_power_loss();
  test_fs_property();
  return CHECK_DONE();
}
