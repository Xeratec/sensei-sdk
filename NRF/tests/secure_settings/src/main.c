/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_e2e on the settings subsystem (flash simulator of native_sim): the stored key names and sizes, the state
 * after a restart (the keys, the counters, the table of senders) and the erase. The logic itself is tested in
 * lib/sensei_secure/tests/test_e2e.c with a fake flash.
 *
 * Two builds (testcase.yaml): plain, and sealed (CONFIG_SENSEI_SECURE_SETTINGS_SEAL with the fixed-key test provider):
 * the private key is the sealed record e2e_privx, a plain e2e_priv of an earlier version is migrated, a record that
 * does not open (changed, wrong name, wrong size, other key) stops the key store without replacing the key, a provider
 * that fails stops it too. */

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include "sensei_e2e_settings.h"
#include "sensei_ratchet.h"
#include "vectors.h" /* lib/sensei_secure/tests/generated: vec_seal (sealed settings records) */

#define PREFIX "sensei_comm" /* default of CONFIG_SENSEI_SECURE_SETTINGS_PREFIX: the keys of the SENSEI IEQ node */
#define STEP 4

static const struct sensei_secure_crypto *C;

struct names {
  size_t len_priv, len_privx, len_rat, len_mrk, len_bpub, len_tx, len_rxs, len_rxf, other;
};

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
#define PRIV_LEN(n) ((n).len_privx)
#define PRIV_STORED (SENSEI_SECURE_PRIV_SIZE + SENSEI_E2E_SEAL_OVERHEAD)
#else
#define PRIV_LEN(n) ((n).len_priv)
#define PRIV_STORED SENSEI_SECURE_PRIV_SIZE
#endif

static int list_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct names *n = param;

  ARG_UNUSED(read_cb);
  ARG_UNUSED(cb_arg);
  if (key == NULL) { /* a setting named exactly like the prefix */
    n->other++;
  } else if (!strcmp(key, "e2e_priv")) {
    n->len_priv = len;
  } else if (!strcmp(key, "e2e_privx")) {
    n->len_privx = len;
  } else if (!strcmp(key, "e2e_rat")) {
    n->len_rat = len;
  } else if (!strcmp(key, "e2e_mrk")) {
    n->len_mrk = len;
  } else if (!strcmp(key, "e2e_bpub")) {
    n->len_bpub = len;
  } else if (!strcmp(key, "e2e_tx")) {
    n->len_tx = len;
  } else if (!strcmp(key, "e2e_rxs")) {
    n->len_rxs = len;
  } else if (!strcmp(key, "e2e_rxf")) {
    n->len_rxf = len;
  } else {
    n->other++;
  }
  return 0;
}

static struct names stored(void) {
  struct names n = {0};

  zassert_equal(settings_load_subtree_direct(PREFIX, list_cb, &n), 0);
  return n;
}

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
static const uint8_t KEY_A[16] = {'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A'};
static const uint8_t KEY_B[16] = {'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B'};
#endif

static void before(void *fixture) {
  ARG_UNUSED(fixture);
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
  sensei_e2e_testkey_set(KEY_A);
  sensei_e2e_settings_seal_close();
#endif
  zassert_equal(sensei_e2e_settings_ops.erase(NULL), 0); /* every test starts from an empty flash */
}

static void *setup(void) {
  C = sensei_secure_crypto_zephyr();
  zassert_not_null(C);
  zassert_equal(settings_subsys_init(), 0);
  return NULL;
}

/* "Reboot": the sealing key is asked for again, the settings are read and the key store starts again. The return codes
 * of both steps go to the caller. */
static void boot_rc(struct sensei_e2e *e, struct sensei_e2e_nv *nv, int *load_rc, int *init_rc) {
  sensei_e2e_settings_seal_close();
  *load_rc = sensei_e2e_settings_load(nv);
  *init_rc = sensei_e2e_init(e, C, &sensei_e2e_settings_ops, NULL, nv, STEP);
}

static void boot(struct sensei_e2e *e) {
  struct sensei_e2e_nv nv;
  int lrc, irc;

  boot_rc(e, &nv, &lrc, &irc);
  zassert_equal(lrc, 0);
  zassert_equal(irc, 0);
}

/* Free the PSA keys of a session (the key slots are few). */
static void stop(struct sensei_e2e *e) {
  if (e->ready) {
    sensei_secure_close(&e->s);
    e->ready = false;
  }
}

static uint32_t counter_of(const uint8_t *env) {
  /* flags, kind, key_id (4), counter (4): no node id, node to backend */
  return (uint32_t)env[6] | ((uint32_t)env[7] << 8) | ((uint32_t)env[8] << 16) | ((uint32_t)env[9] << 24);
}

/* The collector: its key pair and a session as sender 7. */
struct backend {
  uint8_t priv[SENSEI_SECURE_PRIV_SIZE], pub[SENSEI_SECURE_PUB_SIZE];
  struct sensei_secure_keys keys;
  struct sensei_secure s;
};

static void backend_session(struct backend *b, const uint8_t *node_pub) {
  sensei_secure_key_t own = 0;

  zassert_equal(sensei_secure_keygen(C, b->priv, b->pub), 0);
  zassert_equal(C->ecdh_import(C->ctx, b->priv, sizeof(b->priv), &own), 0);
  zassert_equal(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_BACKEND, node_pub, b->pub, &b->keys), 0);
  C->key_destroy(C->ctx, own);
  memset(&b->s, 0, sizeof(b->s));
  zassert_equal(sensei_secure_init(&b->s, C, SENSEI_SECURE_ROLE_BACKEND, &b->keys, 7, 0, 1, NULL, 0, NULL), 0);
}

ZTEST(secure_settings, test_empty_load) {
  struct sensei_e2e_nv nv;

  memset(&nv, 0xA5, sizeof(nv));
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_false(nv.have_priv || nv.have_backend);
  zassert_equal(nv.n_rx, 0);
  zassert_equal(nv.tx_reserved, 0);
  zassert_equal(nv.rx_floor, 0);
}

ZTEST(secure_settings, test_key_names_and_sizes) {
  struct sensei_e2e e;
  struct backend b;
  uint8_t pt[20], env[64];
  struct names n;

  boot(&e);
  n = stored();
  zassert_equal(PRIV_LEN(n), PRIV_STORED);
  zassert_equal(n.len_priv + n.len_privx - PRIV_LEN(n), 0, "only one record of the key");
  zassert_equal(n.len_rat + n.len_bpub + n.len_tx + n.len_rxs + n.len_rxf + n.other, 0);

  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  zassert_true(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  n = stored();
  zassert_equal(n.len_bpub, SENSEI_SECURE_PUB_SIZE);
  zassert_equal(n.len_tx, 4);
  zassert_equal(n.other, 0);
  stop(&e);
  sensei_secure_close(&b.s);
}

ZTEST(secure_settings, test_restart_keeps_keys_and_counters) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint8_t pt[20], env[64];
  uint32_t last = 0;

  boot(&e);
  zassert_false(sensei_e2e_ready(&e));
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  for (int i = 0; i < 6; i++) { /* more than one step: the mark is written */
    zassert_true(sensei_e2e_seal(&e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
    last = counter_of(env);
  }
  zassert_equal(last, 6);

  boot(&e2);
  zassert_true(sensei_e2e_ready(&e2));
  zassert_equal(sensei_e2e_key_id(&e2), sensei_e2e_key_id(&e));
  zassert_mem_equal(e2.node_pub, e.node_pub, sizeof(e.node_pub));
  zassert_true(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  zassert_true(counter_of(env) > last, "counter %u after %u", counter_of(env), last);
  stop(&e);
  stop(&e2);
  sensei_secure_close(&b.s);
}

ZTEST(secure_settings, test_restart_keeps_replay_window) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint8_t env[64], out[64];
  uint8_t pt[8] = {1, 2, 3};
  size_t n;
  int m;

  boot(&e);
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  m = sensei_secure_seal(&b.s, SENSEI_SECURE_KIND_DOWNLINK, NULL, pt, sizeof(pt), env, sizeof(env));
  zassert_true(m > 0);
  zassert_equal(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n), 0);
  zassert_equal(sensei_e2e_senders_used(&e), 1);
  zassert_true(stored().len_rxs > 0);

  stop(&e);
  boot(&e2);
  zassert_equal(sensei_e2e_senders_used(&e2), 1);
  zassert_equal(sensei_e2e_open(&e2, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n), -EALREADY);

  /* clearing the table leaves the floor in flash */
  zassert_equal(sensei_e2e_clear_senders(&e2), 0);
  zassert_equal(sensei_e2e_senders_used(&e2), 0);
  zassert_true(stored().len_rxf == 4);
  stop(&e2);
  boot(&e);
  zassert_equal(sensei_e2e_senders_used(&e), 0);
  zassert_equal(sensei_e2e_open(&e, SENSEI_SECURE_KIND_DOWNLINK, env, (size_t)m, out, sizeof(out), &n), -EALREADY);
  stop(&e);
  sensei_secure_close(&b.s);
}

ZTEST(secure_settings, test_reset_erases) {
  struct sensei_e2e e;
  struct backend b;
  uint8_t old_pub[SENSEI_SECURE_PUB_SIZE];
  struct names n;

  boot(&e);
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  memcpy(old_pub, e.node_pub, sizeof(old_pub));
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_false(sensei_e2e_ready(&e));
  zassert_not_equal(memcmp(old_pub, e.node_pub, sizeof(old_pub)), 0);
  n = stored(); /* only the new key pair */
  zassert_equal(PRIV_LEN(n), PRIV_STORED);
  zassert_equal(n.len_priv + n.len_privx - PRIV_LEN(n), 0);
  zassert_equal(n.len_rat + n.len_bpub + n.len_tx + n.len_rxs + n.len_rxf + n.other, 0);
  stop(&e);
  sensei_secure_close(&b.s);
}

/* ---- the ratchet record e2e_rat (both builds) ---- */

#define RAT_LEN SENSEI_E2E_RAT_SIZE
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
#define RAT_STORED (4 + RAT_LEN + SENSEI_E2E_SEAL_OVERHEAD)
#else
#define RAT_STORED RAT_LEN
#endif

static void rat_pattern(uint8_t *p) {
  for (size_t i = 0; i < RAT_LEN; i++) {
    p[i] = (uint8_t)(0x30 + i * 7);
  }
}

/* The record is written as one entry, read back through the load, and deleted by save_rat(NULL), erase and reset. */
ZTEST(secure_settings, test_rat_record_roundtrip) {
  struct sensei_e2e_nv nv;
  uint8_t plain[RAT_LEN];
  struct names n;
  struct sensei_e2e e;

  rat_pattern(plain);
  boot(&e);
  zassert_equal(stored().len_rat, 0);
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 0x12345678u, plain, RAT_LEN), 0);
  n = stored();
  zassert_equal(n.len_rat, RAT_STORED);
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 0x12345678u, plain, RAT_LEN - 1), -EINVAL);
  zassert_equal(stored().len_rat, RAT_STORED, "a bad length writes nothing");
  stop(&e);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_rat && !nv.rat_unreadable);
  zassert_mem_equal(nv.rat, plain, RAT_LEN);
  /* a second write replaces it (one entry) */
  plain[5] ^= 1;
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 0x12345678u, plain, RAT_LEN), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_rat);
  zassert_mem_equal(nv.rat, plain, RAT_LEN);
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 0, NULL, 0), 0);
  zassert_equal(stored().len_rat, 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_false(nv.have_rat || nv.rat_unreadable);
  /* erase and reset delete it too */
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 1, plain, RAT_LEN), 0);
  zassert_equal(sensei_e2e_settings_ops.erase(NULL), 0);
  zassert_equal(stored().len_rat, 0);
  boot(&e);
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 1, plain, RAT_LEN), 0);
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_equal(stored().len_rat, 0);
  stop(&e);
}

/* A record of the wrong size is lost state, not "no record". */
ZTEST(secure_settings, test_rat_record_of_the_wrong_size_is_unreadable) {
  struct sensei_e2e_nv nv;
  uint8_t junk[RAT_STORED + 8];

  memset(junk, 0x5A, sizeof(junk));
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", junk, RAT_STORED - 1), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.rat_unreadable && !nv.have_rat);
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", junk, RAT_STORED + 1), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.rat_unreadable && !nv.have_rat);
}

/* The mix through the settings backend: the request and the new state survive a restart. */
static void collector_answer(const struct backend *b, const uint8_t *req, size_t rl, uint8_t e_c_pub[32],
                             struct sensei_ratchet_mix *mix, uint32_t *up_base) {
  uint8_t root[32], priv[32];

  zassert_equal(rl, 40u);
  zassert_equal(req[0], 0x72);
  zassert_equal(req[2], 0x08);
  zassert_equal(req[3], 1); /* generation 1 */
  zassert_equal(req[4], 0x12);
  zassert_equal(req[5], 32);
  zassert_equal(req[38], 0x18);
  *up_base = req[39];
  zassert_equal(sensei_ratchet_root0(C, &b->keys, root), 0);
  zassert_equal(C->x25519_keygen(C->ctx, priv, e_c_pub), 0);
  zassert_equal(
      sensei_ratchet_mix(C, SENSEI_SECURE_ROLE_BACKEND, priv, root, b->keys.key_id, 1, *up_base, req + 6, e_c_pub, mix),
      0);
}

ZTEST(secure_settings, test_the_mix_survives_restarts) {
  struct sensei_e2e e, e2, e3;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], req2[SENSEI_E2E_REKEY_MAX], e_c_pub[32], pt[20] = {1}, env[80], out[80];
  struct sensei_ratchet_mix mix;
  struct sensei_ratchet_chain ch;
  sensei_secure_key_t k = 0;
  uint8_t mk[16], nonce[12] = {0};
  uint32_t up_base, ctr;
  size_t n = 0;
  int rl, m;

  boot(&e);
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  rl = sensei_e2e_rekey_due(&e, req, sizeof(req)); /* generation 0 after provisioning: at once */
  zassert_true(rl > 0);
  zassert_equal(stored().len_rat, RAT_STORED, "the request is stored before it is sent");
  stop(&e);

  boot(&e2); /* a restart with the request pending: the same request */
  zassert_equal(sensei_e2e_gen(&e2), 0);
  zassert_true(e2.r.pending);
  zassert_equal(sensei_e2e_rekey_due(&e2, req2, sizeof(req2)), rl);
  zassert_mem_equal(req, req2, (size_t)rl);
  collector_answer(&b, req, (size_t)rl, e_c_pub, &mix, &up_base);
  zassert_equal(sensei_e2e_rekey_answer(&e2, 1, e_c_pub, 32, up_base), 0);
  zassert_equal(sensei_e2e_gen(&e2), 1);
  stop(&e2);

  boot(&e3); /* generation 1 comes back, the node seals under the chain and the collector's keys open it */
  zassert_equal(sensei_e2e_gen(&e3), 1);
  zassert_true(e3.r.prev && !e3.r.pending);
  m = sensei_e2e_seal(&e3, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
  zassert_equal(m, (int)(sizeof(pt) + 26));
  zassert_true(sensei_secure_is_ratchet(env) && sensei_secure_gen_bits(env) == 1);
  ctr = counter_of(env);
  zassert_equal(ctr, 64u * up_base);
  sensei_ratchet_chain_init(&ch, mix.ck, up_base);
  zassert_equal(sensei_ratchet_chain_key(C, &ch, ctr, true, SENSEI_RATCHET_MAX_SKIP, mk), 0);
  zassert_equal(C->aead_import(C->ctx, mk, &k), 0);
  nonce[8] = (uint8_t)ctr;
  nonce[9] = (uint8_t)(ctr >> 8);
  zassert_equal(C->aead_decrypt(C->ctx, k, nonce, env, 10, env + 10, sizeof(pt) + 16, out, sizeof(out), &n), 0);
  zassert_equal(n, sizeof(pt));
  C->key_destroy(C->ctx, k);
  stop(&e3);
  sensei_secure_close(&b.s);
}

/* A record that is there but not valid (or lost): the node refuses to talk until it is provisioned again. */
ZTEST(secure_settings, test_a_damaged_rat_record_stops_the_node_until_it_is_provisioned) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint8_t junk[RAT_STORED], pt[8] = {1}, env[64];

  boot(&e);
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  stop(&e);
  memset(junk, 0x11, sizeof(junk));
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", junk, sizeof(junk)), 0);
  boot(&e2);
  zassert_true(sensei_e2e_ready(&e2) && e2.damaged);
  zassert_equal(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)), -EPERM);
  zassert_equal(sensei_e2e_provision(&e2, b.pub, sizeof(b.pub)), 0);
  zassert_false(e2.damaged);
  zassert_equal(stored().len_rat, 0, "provisioning deletes the record");
  zassert_true(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  stop(&e2);
  sensei_secure_close(&b.s);
}

/* ---- no sealing built in ---- */

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_SEAL

ZTEST(secure_settings, test_without_sealing_a_sealed_record_is_ignored) {
  struct sensei_e2e e;
  uint8_t rec[SENSEI_SECURE_PRIV_SIZE + SENSEI_E2E_SEAL_OVERHEAD] = {1};
  uint8_t out[8];
  struct names n;

  zassert_false(sensei_e2e_settings_sealed());
  zassert_equal(sensei_e2e_settings_seal_status(), -ENOTSUP);
  zassert_equal(sensei_e2e_settings_seal("e2e_privx", out, 4, rec, sizeof(rec)), -ENOTSUP);
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, sizeof(rec), out, sizeof(out)), -ENOTSUP);
  zassert_equal(settings_save_one(PREFIX "/e2e_privx", rec, sizeof(rec)), 0);
  boot(&e); /* makes its own plain key pair; the record of another build stays where it is */
  n = stored();
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE);
  zassert_equal(n.len_privx, sizeof(rec));
  stop(&e);
}

#endif

/* ---- sealing ---- */

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL

#define SEALED_LEN (SENSEI_SECURE_PRIV_SIZE + SENSEI_E2E_SEAL_OVERHEAD)

struct raw {
  const char *name;
  uint8_t buf[256];
  size_t len;
};

static int raw_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct raw *r = param;

  if (key != NULL && !strcmp(key, r->name) && len <= sizeof(r->buf)) {
    r->len = read_cb(cb_arg, r->buf, len) == (ssize_t)len ? len : 0;
  }
  return 0;
}

/* The bytes of a stored record (0 if there is none). */
static size_t read_raw(const char *name, uint8_t *buf) {
  struct raw r = {.name = name};

  zassert_equal(settings_load_subtree_direct(PREFIX, raw_cb, &r), 0);
  memcpy(buf, r.buf, r.len);
  return r.len;
}

static bool contains(const uint8_t *buf, size_t len, const uint8_t *pat, size_t n) {
  for (size_t i = 0; i + n <= len; i++) {
    if (!memcmp(buf + i, pat, n)) {
      return true;
    }
  }
  return false;
}

/* Does the whole settings partition (every page, also the stale records) hold these bytes? */
static bool flash_has(const uint8_t *pat, size_t n) {
  const struct flash_area *fa;
  uint8_t page[128];
  bool found = false;

  zassert_equal(flash_area_open(PARTITION_ID(storage_partition), &fa), 0);
  for (off_t off = 0; off + (off_t)sizeof(page) <= (off_t)fa->fa_size && !found; off += sizeof(page) - n) {
    zassert_equal(flash_area_read(fa, off, page, sizeof(page)), 0);
    found = contains(page, sizeof(page), pat, n);
  }
  flash_area_close(fa);
  return found;
}

ZTEST(secure_settings, test_sealed_names_sizes_and_no_plain_copy) {
  struct sensei_e2e e;
  uint8_t rec[SEALED_LEN], plain[SENSEI_SECURE_PRIV_SIZE], other[SEALED_LEN];
  struct names n;

  zassert_true(sensei_e2e_settings_sealed());
  boot(&e);
  zassert_equal(sensei_e2e_settings_seal_status(), 0);
  n = stored();
  zassert_equal(n.len_privx, SEALED_LEN); /* 1 + 12 + 32 + 16 = 61 */
  zassert_equal(n.len_privx, 61);
  zassert_equal(n.len_priv, 0, "no plain key next to the sealed one");
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  zassert_equal(rec[0], SENSEI_E2E_SEAL_VERSION);
  zassert_false(contains(rec, sizeof(rec), e.nv.priv, sizeof(e.nv.priv)), "the key is not in the record");
  zassert_false(flash_has(e.nv.priv, sizeof(e.nv.priv)), "the key was never written in the clear");

  /* The record opens under its own name only, with the key of the provider. */
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, sizeof(rec), plain, sizeof(plain)), 32);
  zassert_mem_equal(plain, e.nv.priv, sizeof(plain));
  zassert_equal(sensei_e2e_settings_unseal("e2e_rat", rec, sizeof(rec), plain, sizeof(plain)), -EACCES);
  zassert_equal(sensei_e2e_settings_unseal("e2e_priv", rec, sizeof(rec), plain, sizeof(plain)), -EACCES);
  /* length, version, name */
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, 28, plain, sizeof(plain)), -EBADMSG);
  memcpy(other, rec, sizeof(rec));
  other[0] = 2;
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", other, sizeof(other), plain, sizeof(plain)), -EBADMSG);
  zassert_equal(sensei_e2e_settings_seal("", plain, 4, other, sizeof(other)), -EINVAL);
  zassert_equal(sensei_e2e_settings_seal("e2e_privx", plain, 4, other, 10), -ENOSPC);
  stop(&e);
}

/* Vector 7: the records of the Python implementation, made and opened by the PSA backend. */
ZTEST(secure_settings, test_sealed_record_vectors_on_psa) {
  sensei_secure_key_t k = 0;

  zassert_equal(C->aead_import(C->ctx, vec_seal_key, &k), 0);
  for (size_t i = 0; i < VEC_SEAL_COUNT; i++) {
    const vec_seal_t *v = &vec_seal[i];
    uint8_t rec[256], plain[256];
    int n = sensei_e2e_seal_record_ctx(C, k, v->name, v->ctx, v->ctx_len, v->nonce, v->plain, v->plain_len, rec,
                                       sizeof(rec));

    zassert_equal(n, (int)v->record_len);
    zassert_mem_equal(rec, v->record, v->record_len);
    n = sensei_e2e_open_record_ctx(C, k, v->name, v->ctx, v->ctx_len, v->record, v->record_len, plain, sizeof(plain));
    zassert_equal(n, (int)v->plain_len);
    zassert_mem_equal(plain, v->plain, v->plain_len);
    zassert_equal(sensei_e2e_open_record_ctx(C, k, "e2e_other", v->ctx, v->ctx_len, v->record, v->record_len, plain,
                                             sizeof(plain)),
                  -EACCES);
  }
  C->key_destroy(C->ctx, k);
}

ZTEST(secure_settings, test_every_seal_uses_a_new_nonce) {
  uint8_t a[SEALED_LEN], b[SEALED_LEN], pt[SENSEI_SECURE_PRIV_SIZE] = {1, 2, 3};

  zassert_equal(sensei_e2e_settings_seal("e2e_privx", pt, sizeof(pt), a, sizeof(a)), SEALED_LEN);
  zassert_equal(sensei_e2e_settings_seal("e2e_privx", pt, sizeof(pt), b, sizeof(b)), SEALED_LEN);
  zassert_not_equal(memcmp(a + 1, b + 1, SENSEI_E2E_SEAL_NONCE_SIZE), 0);
  zassert_not_equal(memcmp(a + 13, b + 13, sizeof(pt)), 0);
}

ZTEST(secure_settings, test_restart_opens_the_same_key) {
  struct sensei_e2e e, e2;
  uint8_t rec1[SEALED_LEN], rec2[SEALED_LEN];

  boot(&e);
  zassert_equal(read_raw("e2e_privx", rec1), SEALED_LEN);
  boot(&e2);
  zassert_mem_equal(e2.node_pub, e.node_pub, sizeof(e.node_pub));
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_mem_equal(rec1, rec2, SEALED_LEN, "a restart does not write the record again");
  stop(&e);
  stop(&e2);
}

ZTEST(secure_settings, test_reset_seals_a_new_key_with_a_new_nonce) {
  struct sensei_e2e e;
  uint8_t rec1[SEALED_LEN], rec2[SEALED_LEN];

  boot(&e);
  zassert_equal(read_raw("e2e_privx", rec1), SEALED_LEN);
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_not_equal(memcmp(rec1 + 1, rec2 + 1, SENSEI_E2E_SEAL_NONCE_SIZE), 0);
  zassert_false(flash_has(e.nv.priv, sizeof(e.nv.priv)));
  stop(&e);
}

/* A record that does not open: the key store does not start, nothing is replaced, reset is the way out. */
static void expect_unreadable(const uint8_t *rec_before, size_t len_before) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  uint8_t rec[SEALED_LEN + 8];
  int lrc, irc;
  struct names n;

  boot_rc(&e, &nv, &lrc, &irc);
  zassert_true(lrc < 0, "load %d", lrc);
  zassert_true(nv.key_unreadable);
  zassert_false(nv.have_priv);
  zassert_equal(irc, -EACCES);
  zassert_false(sensei_e2e_ready(&e));
  zassert_equal(sensei_e2e_key_id(&e), 0);
  n = stored(); /* the stored record is as it was: no new key pair over it */
  zassert_equal(n.len_privx, len_before);
  zassert_equal(n.len_priv, 0);
  zassert_equal(read_raw("e2e_privx", rec), len_before);
  zassert_mem_equal(rec, rec_before, len_before);
  /* reset starts over */
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_equal(stored().len_privx, SEALED_LEN);
  boot(&e);
  zassert_true(e.nv.have_priv);
  stop(&e);
}

ZTEST(secure_settings, test_tampered_record_is_refused) {
  struct sensei_e2e e;
  uint8_t rec[SEALED_LEN];

  boot(&e);
  stop(&e);
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  rec[20] ^= 0x01; /* a bit of the ciphertext */
  zassert_equal(settings_save_one(PREFIX "/e2e_privx", rec, sizeof(rec)), 0);
  expect_unreadable(rec, sizeof(rec));
}

ZTEST(secure_settings, test_tampered_nonce_tag_and_version_are_refused) {
  struct sensei_e2e e;
  uint8_t good[SEALED_LEN], rec[SEALED_LEN];
  static const size_t at[] = {0, 1, 12, 13, 44, 45, 60};

  boot(&e);
  stop(&e);
  zassert_equal(read_raw("e2e_privx", good), SEALED_LEN);
  for (size_t i = 0; i < ARRAY_SIZE(at); i++) {
    memcpy(rec, good, sizeof(rec));
    rec[at[i]] ^= 0x80;
    zassert_equal(settings_save_one(PREFIX "/e2e_privx", rec, sizeof(rec)), 0);
    expect_unreadable(rec, sizeof(rec)); /* it also resets the key store, so the next round starts from a good one */
  }
}

ZTEST(secure_settings, test_record_of_another_name_or_size_is_refused) {
  struct sensei_e2e e;
  uint8_t rec[SEALED_LEN + 4];
  uint8_t other_name[SEALED_LEN];

  boot(&e);
  /* a record sealed for another name, put in the place of the key */
  zassert_equal(sensei_e2e_settings_seal("e2e_rat", e.nv.priv, sizeof(e.nv.priv), other_name, sizeof(other_name)),
                SEALED_LEN);
  zassert_equal(settings_save_one(PREFIX "/e2e_privx", other_name, sizeof(other_name)), 0);
  stop(&e);
  expect_unreadable(other_name, sizeof(other_name));

  /* a record of the wrong size */
  boot(&e);
  memset(rec, 0x5A, sizeof(rec));
  zassert_equal(settings_save_one(PREFIX "/e2e_privx", rec, SEALED_LEN - 1), 0);
  stop(&e);
  expect_unreadable(rec, SEALED_LEN - 1);
}

ZTEST(secure_settings, test_a_flash_image_on_another_chip_does_not_open) {
  struct sensei_e2e e;
  uint8_t rec[SEALED_LEN];

  boot(&e);
  stop(&e);
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  sensei_e2e_testkey_set(KEY_B); /* the key of another chip */
  expect_unreadable(rec, sizeof(rec));
}

ZTEST(secure_settings, test_provider_failure_on_first_start_stores_nothing) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  int lrc, irc;
  struct names n;

  sensei_e2e_testkey_set(NULL);
  boot_rc(&e, &nv, &lrc, &irc);
  zassert_equal(lrc, 0, "nothing stored: nothing to open");
  zassert_equal(irc, -EIO);
  zassert_false(sensei_e2e_ready(&e));
  zassert_false(e.nv.have_priv);
  zassert_equal(sensei_e2e_settings_seal_status(), -EIO, "the failure is remembered");
  n = stored();
  zassert_equal(n.len_priv + n.len_privx + n.len_bpub + n.len_tx, 0);

  /* with a key it works (the provider is asked again at the next start) */
  sensei_e2e_testkey_set(KEY_A);
  boot(&e);
  zassert_equal(stored().len_privx, SEALED_LEN);
  stop(&e);
}

/* The key store of an earlier version: plain e2e_priv, collector key, counters. */
static void write_plain_state(struct sensei_e2e *e, struct backend *b, uint32_t *key_id, uint8_t *pub) {
  uint8_t pt[20], env[64];

  boot(e);
  backend_session(b, e->node_pub);
  zassert_equal(sensei_e2e_provision(e, b->pub, sizeof(b->pub)), 0);
  for (int i = 0; i < 6; i++) {
    zassert_true(sensei_e2e_seal(e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  }
  *key_id = sensei_e2e_key_id(e);
  memcpy(pub, e->node_pub, SENSEI_SECURE_PUB_SIZE);
  /* go back to the old layout: the key in the clear, no sealed record */
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", e->nv.priv, SENSEI_SECURE_PRIV_SIZE), 0);
  zassert_equal(settings_delete(PREFIX "/e2e_privx"), 0);
  stop(e);
}

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD /* the plain record stays until the confirmation */
ZTEST(secure_settings, test_migration_of_the_plain_key) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint32_t key_id;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE], priv[SENSEI_SECURE_PRIV_SIZE], rec[SEALED_LEN], opened[32], pt[20], env[64];
  struct names n;

  write_plain_state(&e, &b, &key_id, pub);
  memcpy(priv, e.nv.priv, sizeof(priv));
  n = stored();
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE);
  zassert_equal(n.len_privx, 0);

  boot(&e2); /* the first start of the new firmware */
  n = stored();
  zassert_equal(n.len_privx, SEALED_LEN, "sealed");
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE, "the plain record stays until the image is confirmed");
  zassert_true(sensei_e2e_settings_plain_pending());
  zassert_equal(n.len_bpub, SENSEI_SECURE_PUB_SIZE, "the other records stay");
  zassert_true(sensei_e2e_ready(&e2));
  zassert_equal(sensei_e2e_key_id(&e2), key_id, "the key id does not change");
  zassert_mem_equal(e2.node_pub, pub, sizeof(pub));
  zassert_mem_equal(e2.nv.priv, priv, sizeof(priv));
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, sizeof(rec), opened, sizeof(opened)), 32);
  zassert_mem_equal(opened, priv, sizeof(priv));
  /* the collector still talks to it, the counters went on */
  zassert_true(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, 20, env, sizeof(env)) > 0);
  zassert_true(counter_of(env) > 6);
  stop(&e2);

  /* a restart before the confirmation (the image is on trial, or goes back): the plain record is still there, and an
   * old image that reads it finds the same key */
  boot(&e);
  zassert_mem_equal(e.node_pub, pub, sizeof(pub));
  zassert_equal(stored().len_priv, SENSEI_SECURE_PRIV_SIZE);
  zassert_equal(read_raw("e2e_priv", opened), SENSEI_SECURE_PRIV_SIZE);
  zassert_mem_equal(opened, priv, sizeof(priv));
  zassert_true(sensei_e2e_settings_plain_pending());
  stop(&e);

  /* the confirmation deletes it; the sealed key stays and is the identity of the next start */
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_false(sensei_e2e_settings_plain_pending());
  n = stored();
  zassert_equal(n.len_priv, 0);
  zassert_equal(n.len_privx, SEALED_LEN);
  zassert_equal(sensei_e2e_settings_confirm(), 0, "nothing to delete: fine");
  boot(&e);
  zassert_mem_equal(e.node_pub, pub, sizeof(pub));
  zassert_false(sensei_e2e_settings_plain_pending());
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  stop(&e);
  sensei_secure_close(&b.s);
}
#endif

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD /* the plain record stays until the confirmation */
/* The confirmation is for the key that is loaded: a reset in between leaves nothing to delete, and no plain key. */
ZTEST(secure_settings, test_confirm_after_a_reset_and_without_a_load) {
  struct sensei_e2e e;
  struct backend b;
  uint32_t key_id;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE];

  zassert_equal(sensei_e2e_settings_confirm(), 0);
  write_plain_state(&e, &b, &key_id, pub);
  boot(&e);
  zassert_true(sensei_e2e_settings_plain_pending());
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_false(sensei_e2e_settings_plain_pending());
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  stop(&e);
  sensei_secure_close(&b.s);
}
#endif

/* A plain key that is not a valid scalar is damaged: it must not be sealed and kept; the key store replaces it, the
 * new key is sealed, the plain record is gone. (Sealing it first would have made the replacement refuse: a sealed key
 * that does not give a public key is taken for a crypto error.) */
ZTEST(secure_settings, test_a_damaged_plain_key_is_replaced_not_migrated) {
  struct sensei_e2e e, e2;
  struct sensei_e2e_nv nv;
  static const uint8_t zero[SENSEI_SECURE_PRIV_SIZE];
  static const uint8_t big[SENSEI_SECURE_PRIV_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t *bad[] = {zero, big};
  uint8_t rec[SEALED_LEN], opened[32];
  int lrc;
  struct names n;

  for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
    zassert_equal(settings_save_one(PREFIX "/e2e_priv", bad[i], SENSEI_SECURE_PRIV_SIZE), 0);
    sensei_e2e_settings_seal_close();
    lrc = sensei_e2e_settings_load(&nv);
    zassert_equal(lrc, 0);
    zassert_true(nv.have_priv);
    zassert_false(nv.priv_sealed, "a damaged key is not a sealed one");
    zassert_false(sensei_e2e_settings_plain_pending());
    zassert_equal(sensei_e2e_init(&e, C, &sensei_e2e_settings_ops, NULL, &nv, STEP), 0);
    n = stored();
    zassert_equal(n.len_privx, SEALED_LEN, "the new key is sealed");
    zassert_equal(n.len_priv, 0, "the damaged record is gone");
    zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
    zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, sizeof(rec), opened, sizeof(opened)), 32);
    zassert_mem_equal(opened, e.nv.priv, sizeof(opened));
    zassert_true(sensei_secure_priv_valid(opened));
    boot(&e2); /* and it is the key of the next start */
    zassert_mem_equal(e2.node_pub, e.node_pub, sizeof(e.node_pub));
    stop(&e);
    stop(&e2);
    zassert_equal(sensei_e2e_settings_ops.erase(NULL), 0);
  }
}

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD /* the plain record stays until the confirmation */
/* A damaged plain record next to a good sealed key: the sealed key stands; the confirmation deletes the junk. */
ZTEST(secure_settings, test_a_damaged_plain_key_next_to_a_sealed_key_is_junk) {
  struct sensei_e2e e, e2;
  static const uint8_t zero[SENSEI_SECURE_PRIV_SIZE];

  boot(&e);
  stop(&e);
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", zero, sizeof(zero)), 0);
  boot(&e2);
  zassert_mem_equal(e2.node_pub, e.node_pub, sizeof(e.node_pub));
  zassert_true(sensei_e2e_settings_plain_pending());
  zassert_equal(stored().len_priv, SENSEI_SECURE_PRIV_SIZE);
  stop(&e2);
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  zassert_equal(stored().len_privx, SEALED_LEN);
}
#endif

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD /* the plain record stays until the confirmation */
/* The migration stopped between the two writes: the sealed record is there, the same plain key was not deleted yet. */
ZTEST(secure_settings, test_migration_stopped_between_the_two_writes) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint32_t key_id;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE], rec1[SEALED_LEN], rec2[SEALED_LEN];
  struct names n;

  write_plain_state(&e, &b, &key_id, pub);
  boot(&e2); /* the migration */
  stop(&e2);
  zassert_equal(read_raw("e2e_privx", rec1), SEALED_LEN);
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", e2.nv.priv, SENSEI_SECURE_PRIV_SIZE), 0);
  boot(&e2);
  n = stored();
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE, "the plain copy of the same key stays until the confirmation");
  zassert_equal(n.len_privx, SEALED_LEN);
  zassert_true(sensei_e2e_settings_plain_pending());
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_mem_equal(rec1, rec2, SEALED_LEN, "the sealed record is not written again");
  zassert_mem_equal(e2.node_pub, pub, sizeof(pub));
  zassert_equal(sensei_e2e_key_id(&e2), key_id);
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  stop(&e2);
  sensei_secure_close(&b.s);
}
#endif

#ifndef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD /* the plain record stays until the confirmation */
/* Downgrade, then upgrade: the sealed key A is there, the old firmware made a new plain key B (its reset does not know
 * e2e_privx) and the collector was provisioned for B. The plain key is the newer one: it is sealed over A. */
ZTEST(secure_settings, test_downgrade_then_upgrade_keeps_the_newer_plain_key) {
  struct sensei_e2e e, e2, e3;
  struct backend b;
  uint8_t priv_b[SENSEI_SECURE_PRIV_SIZE], pub_b[SENSEI_SECURE_PUB_SIZE], rec[SEALED_LEN], opened[32], pt[20], env[64];
  struct names n;

  boot(&e); /* key A, sealed */
  stop(&e);
  zassert_equal(sensei_secure_keygen(C, priv_b, pub_b), 0);
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", priv_b, sizeof(priv_b)), 0);
  zassert_equal(memcmp(pub_b, e.node_pub, sizeof(pub_b)) != 0, true);

  boot(&e2);
  zassert_mem_equal(e2.node_pub, pub_b, sizeof(pub_b), "the identity is the plain key B, not the old A");
  n = stored();
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE, "the plain record stays until the confirmation");
  zassert_equal(n.len_privx, SEALED_LEN);
  zassert_equal(read_raw("e2e_privx", rec), SEALED_LEN);
  zassert_equal(sensei_e2e_settings_unseal("e2e_privx", rec, sizeof(rec), opened, sizeof(opened)), 32);
  zassert_mem_equal(opened, priv_b, sizeof(priv_b), "B is sealed now");
  backend_session(&b, e2.node_pub);
  zassert_equal(sensei_e2e_provision(&e2, b.pub, sizeof(b.pub)), 0);
  zassert_true(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  stop(&e2);

  boot(&e3); /* and it stays B, also before the confirmation */
  zassert_mem_equal(e3.node_pub, pub_b, sizeof(pub_b));
  stop(&e3);
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  sensei_secure_close(&b.s);
}
#endif

static int fail_import(void *ctx, const uint8_t *priv, size_t len, sensei_secure_key_t *key) {
  ARG_UNUSED(ctx);
  ARG_UNUSED(priv);
  ARG_UNUSED(len);
  ARG_UNUSED(key);
  return -ENOMEM; /* no free key slot */
}

/* The sealed key opens, but the public key cannot be derived (a crypto error such as a full key store): the stored
 * record is not replaced by a new key pair, e2e stays off. */
ZTEST(secure_settings, test_a_crypto_error_does_not_replace_the_sealed_key) {
  struct sensei_e2e e, e2, e3;
  struct sensei_e2e_nv nv;
  struct sensei_secure_crypto bad = *sensei_secure_crypto_zephyr();
  uint8_t rec1[SEALED_LEN], rec2[SEALED_LEN];

  boot(&e);
  stop(&e);
  zassert_equal(read_raw("e2e_privx", rec1), SEALED_LEN);

  bad.ecdh_import = fail_import;
  sensei_e2e_settings_seal_close();
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_priv);
  zassert_true(nv.priv_sealed);
  zassert_equal(sensei_e2e_init(&e2, &bad, &sensei_e2e_settings_ops, NULL, &nv, STEP), -EIO);
  zassert_false(sensei_e2e_ready(&e2));
  zassert_false(e2.nv.have_priv);
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_mem_equal(rec1, rec2, SEALED_LEN, "the sealed key is untouched");

  boot(&e3); /* the next start, with a working import, has the same key */
  zassert_mem_equal(e3.node_pub, e.node_pub, sizeof(e.node_pub));
  stop(&e3);
}

/* sensei_e2e_settings_reset_check(): a reset erases the key first, so it must not start while the sealing key is
 * missing; a failure cached at the start must not block a reset when the key is back. */
ZTEST(secure_settings, test_reset_check_needs_the_sealing_key) {
  struct sensei_e2e e;
  struct sensei_e2e_nv nv;
  uint8_t rec1[SEALED_LEN], rec2[SEALED_LEN];
  int lrc, irc;

  boot(&e);
  stop(&e);
  zassert_equal(read_raw("e2e_privx", rec1), SEALED_LEN);

  sensei_e2e_testkey_set(NULL); /* the sealing key is not available at the start */
  boot_rc(&e, &nv, &lrc, &irc);
  zassert_equal(irc, -EACCES);
  zassert_equal(sensei_e2e_settings_reset_check(), -EIO, "no sealing key: the reset must not start");
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_mem_equal(rec1, rec2, SEALED_LEN, "the check does not touch the key");

  sensei_e2e_testkey_set(KEY_A); /* the key is back; the failure was cached until the check asks again */
  zassert_equal(sensei_e2e_settings_reset_check(), 0);
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_equal(read_raw("e2e_privx", rec2), SEALED_LEN);
  zassert_not_equal(memcmp(rec1 + 1, rec2 + 1, SENSEI_E2E_SEAL_NONCE_SIZE), 0, "a new key is sealed");
  stop(&e);
}

ZTEST(secure_settings, test_seal_prepare_needs_nothing_from_the_test_provider) {
  zassert_equal(sensei_e2e_settings_seal_prepare(), 0);
}

/* A setting named exactly like the prefix has no key name: the loader must not use it. */
ZTEST(secure_settings, test_a_setting_named_like_the_prefix_is_harmless) {
  struct sensei_e2e e, e2;
  uint8_t other[4] = {1, 2, 3, 4};

  boot(&e);
  stop(&e);
  /* the plain layout again, so that the migration reads back the subtree */
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", e.nv.priv, SENSEI_SECURE_PRIV_SIZE), 0);
  zassert_equal(settings_delete(PREFIX "/e2e_privx"), 0);
  zassert_equal(settings_save_one(PREFIX, other, sizeof(other)), 0);
  boot(&e2);
  zassert_mem_equal(e2.node_pub, e.node_pub, sizeof(e.node_pub));
  zassert_equal(stored().len_privx, SEALED_LEN);
  stop(&e2);
  zassert_equal(settings_delete(PREFIX), 0);
}

ZTEST(secure_settings, test_migration_with_a_failing_provider_keeps_the_plain_key) {
  struct sensei_e2e e, e2;
  struct sensei_e2e_nv nv;
  struct backend b;
  uint32_t key_id;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE];
  int lrc, irc;
  struct names n;

  write_plain_state(&e, &b, &key_id, pub);
  sensei_e2e_testkey_set(NULL);
  boot_rc(&e2, &nv, &lrc, &irc);
  zassert_equal(lrc, -EIO);
  zassert_true(nv.key_unreadable);
  zassert_false(nv.have_priv);
  zassert_equal(irc, -EACCES);
  zassert_false(sensei_e2e_ready(&e2)); /* e2e is off, visible to the caller */
  n = stored();
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE, "the plain key stays: nothing is lost");
  zassert_equal(n.len_privx, 0);

  /* the next start with a working provider migrates */
  sensei_e2e_testkey_set(KEY_A);
  boot(&e2);
  zassert_equal(sensei_e2e_key_id(&e2), key_id);
  n = stored();
  zassert_equal(n.len_privx, SEALED_LEN);
  zassert_equal(n.len_priv, IS_ENABLED(CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD) ? 0 : SENSEI_SECURE_PRIV_SIZE,
                "kept until the confirmation");
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  stop(&e2);
  sensei_secure_close(&b.s);
}

/* e2e_rat sealed: key_id (4 B, plain) || sealed record with the key id as context. */
ZTEST(secure_settings, test_the_rat_record_is_sealed_and_bound_to_the_key_id) {
  struct sensei_e2e_nv nv;
  uint8_t plain[RAT_LEN], raw[RAT_STORED + 4], bad[RAT_STORED], other[RAT_STORED];
  struct names n;
  struct sensei_e2e e;
  int u;

  boot(&e); /* a key record to take as the record of another name below */
  stop(&e);
  rat_pattern(plain);
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 0x12345678u, plain, RAT_LEN), 0);
  n = stored();
  zassert_equal(n.len_rat, 4 + RAT_LEN + SENSEI_E2E_SEAL_OVERHEAD);
  zassert_equal(n.len_rat, 189);
  zassert_equal(read_raw("e2e_rat", raw), RAT_STORED);
  zassert_equal(raw[0], 0x78);
  zassert_equal(raw[1], 0x56);
  zassert_equal(raw[2], 0x34);
  zassert_equal(raw[3], 0x12);
  zassert_equal(raw[4], SENSEI_E2E_SEAL_VERSION);
  zassert_false(contains(raw, RAT_STORED, plain + 16, 32), "the secrets are not in the record");
  zassert_false(flash_has(plain + 16, 32), "and never in the clear in flash");
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_rat && !nv.rat_unreadable);
  zassert_mem_equal(nv.rat, plain, RAT_LEN);
  /* the key id in front and in the associated data: change either and it does not open */
  memcpy(bad, raw, RAT_STORED);
  bad[0] ^= 1;
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", bad, RAT_STORED), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.rat_unreadable && !nv.have_rat);
  /* every other byte too: the version, the nonce, the ciphertext, the tag */
  for (size_t i = 4; i < RAT_STORED; i += 17) {
    memcpy(bad, raw, RAT_STORED);
    bad[i] ^= 0x40;
    zassert_equal(settings_save_one(PREFIX "/e2e_rat", bad, RAT_STORED), 0);
    zassert_equal(sensei_e2e_settings_load(&nv), 0);
    zassert_true(nv.rat_unreadable && !nv.have_rat, "byte %zu", i);
  }
  /* a record sealed under another key id (the sealed part moved behind a different prefix) */
  u = sensei_e2e_settings_seal_ctx("e2e_rat", (const uint8_t[]){1, 2, 3, 4}, 4, plain, RAT_LEN, other + 4,
                                   sizeof(other) - 4);
  zassert_equal(u, (int)(RAT_LEN + SENSEI_E2E_SEAL_OVERHEAD));
  memcpy(other, (const uint8_t[]){9, 9, 9, 9}, 4);
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", other, 4 + (size_t)u), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.rat_unreadable);
  memcpy(other, (const uint8_t[]){1, 2, 3, 4}, 4);
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", other, 4 + (size_t)u), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_rat && !nv.rat_unreadable);
  /* the record of the key (another name) in its place */
  zassert_equal(read_raw("e2e_privx", bad), SEALED_LEN);
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", bad, SEALED_LEN), 0);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.rat_unreadable);
  /* the key of another chip: the key record does not open either, the key store does not start */
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, 7, plain, RAT_LEN), 0);
  sensei_e2e_testkey_set(KEY_B);
  sensei_e2e_settings_seal_close();
  zassert_true(sensei_e2e_settings_load(&nv) < 0);
  zassert_true(nv.key_unreadable && !nv.have_rat);
}

/* A record of another key id than the key pair of the session: old (a reset or a new key pair left it), ignored. */
ZTEST(secure_settings, test_a_rat_record_of_another_key_id_is_ignored_by_the_key_store) {
  struct sensei_e2e e, e2;
  struct sensei_e2e_nv nv;
  struct backend b;
  uint8_t req[SENSEI_E2E_REKEY_MAX], pt[8] = {1}, env[64];
  uint32_t id;

  boot(&e);
  backend_session(&b, e.node_pub);
  zassert_equal(sensei_e2e_provision(&e, b.pub, sizeof(b.pub)), 0);
  zassert_true(sensei_e2e_rekey_due(&e, req, sizeof(req)) > 0); /* a valid record: generation 0, pending */
  id = sensei_e2e_key_id(&e);
  stop(&e);
  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_true(nv.have_rat);
  nv.rat[4] ^= 1; /* the key id inside (the first byte of it) and in front */
  zassert_equal(sensei_e2e_settings_ops.save_rat(NULL, id ^ 1u, nv.rat, RAT_LEN), 0);
  boot(&e2);
  zassert_false(e2.damaged);
  zassert_equal(sensei_e2e_gen(&e2), 0);
  zassert_false(e2.r.pending, "the old record is not used");
  zassert_true(sensei_e2e_seal(&e2, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env)) > 0);
  stop(&e2);
  sensei_secure_close(&b.s);
}

/* The marker e2e_mrk: 4 bytes of a hash of the sealed key, written whenever a key is sealed. */
static void marker_of(const uint8_t *priv, uint8_t fp[4]) {
  static const uint8_t salt[] = "SENSEI-E2E-mrk-v1";

  zassert_equal(C->hkdf(C->ctx, priv, 32, salt, sizeof(salt) - 1, NULL, 0, fp, 4), 0);
}

static void expect_marker_of(const uint8_t *priv) {
  uint8_t fp[4], got[8];

  marker_of(priv, fp);
  zassert_equal(read_raw("e2e_mrk", got), 4u);
  zassert_mem_equal(got, fp, 4);
}

ZTEST(secure_settings, test_the_marker_follows_the_sealed_key) {
  struct sensei_e2e e;
  uint8_t old[32];

  boot(&e);
  zassert_equal(stored().len_mrk, 4u);
  expect_marker_of(e.nv.priv);
  memcpy(old, e.nv.priv, sizeof(old));
  zassert_equal(sensei_e2e_reset(&e), 0); /* a new key: a new marker */
  zassert_not_equal(memcmp(old, e.nv.priv, sizeof(old)), 0);
  expect_marker_of(e.nv.priv);
  stop(&e);
  zassert_equal(sensei_e2e_settings_ops.erase(NULL), 0);
  zassert_equal(stored().len_mrk, 0u, "erase deletes it");
}

/* The first keys-at-rest image seals first and ignores a failed delete: a newer sealed key can sit next to an older
 * plain one. The marker still names the key that was sealed before: the sealed key stands, the plain record goes at
 * once. */
ZTEST(secure_settings, test_an_older_plain_key_next_to_a_newer_sealed_key_loses) {
  struct sensei_e2e e, e2, e3;
  uint8_t priv2[32], pub2[65], rec[SEALED_LEN], old_plain[32];
  struct names n;

  boot(&e); /* key 1, sealed, marker of key 1 */
  memcpy(old_plain, e.nv.priv, sizeof(old_plain));
  stop(&e);
  zassert_equal(sensei_secure_keygen(C, priv2, pub2), 0);
  zassert_equal(sensei_e2e_settings_seal("e2e_privx", priv2, sizeof(priv2), rec, sizeof(rec)), SEALED_LEN);
  zassert_equal(settings_save_one(PREFIX "/e2e_privx", rec, sizeof(rec)), 0); /* what the old image wrote: no marker */
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", old_plain, sizeof(old_plain)), 0); /* the failed delete */
  boot(&e2);
  zassert_mem_equal(e2.node_pub, pub2, sizeof(pub2), "the identity is the sealed key 2");
  n = stored();
  zassert_equal(n.len_priv, 0, "the older plain key is deleted at once");
  zassert_false(sensei_e2e_settings_plain_pending());
  expect_marker_of(priv2);
  stop(&e2);
  boot(&e3);
  zassert_mem_equal(e3.node_pub, pub2, sizeof(pub2));
  stop(&e3);
}

/* No marker at all (a node that went through the first keys-at-rest image only): the plain key is taken for the newer
 * one, as before the marker existed; the marker is written then. */
ZTEST(secure_settings, test_without_a_marker_the_plain_key_is_the_newer_one) {
  struct sensei_e2e e, e2;
  uint8_t priv_b[32], pub_b[65];

  boot(&e);
  stop(&e);
  zassert_equal(settings_delete(PREFIX "/e2e_mrk"), 0);
  zassert_equal(sensei_secure_keygen(C, priv_b, pub_b), 0);
  zassert_equal(settings_save_one(PREFIX "/e2e_priv", priv_b, sizeof(priv_b)), 0);
  boot(&e2);
  zassert_mem_equal(e2.node_pub, pub_b, sizeof(pub_b));
  expect_marker_of(priv_b);
  stop(&e2);
}

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD
/* No bootloader that reverts: the plain record is deleted right after the load. */
ZTEST(secure_settings, test_without_mcuboot_the_plain_record_goes_after_the_load) {
  struct sensei_e2e e, e2;
  struct backend b;
  uint32_t key_id;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE];
  struct names n;

  write_plain_state(&e, &b, &key_id, pub);
  zassert_equal(stored().len_priv, SENSEI_SECURE_PRIV_SIZE);
  boot(&e2);
  n = stored();
  zassert_equal(n.len_privx, SEALED_LEN);
  zassert_equal(n.len_priv, 0);
  zassert_false(sensei_e2e_settings_plain_pending());
  zassert_equal(sensei_e2e_key_id(&e2), key_id);
  zassert_mem_equal(e2.node_pub, pub, sizeof(pub));
  stop(&e2);
  sensei_secure_close(&b.s);
}
#endif
#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

ZTEST_SUITE(secure_settings, NULL, setup, before, NULL, NULL);
