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
#include "vectors.h" /* lib/sensei_secure/tests/generated: vec_seal (sealed settings records) */

#define PREFIX "sensei_comm" /* default of CONFIG_SENSEI_SECURE_SETTINGS_PREFIX: the keys of the SENSEI IEQ node */
#define STEP 4

static const struct sensei_secure_crypto *C;

struct names {
  size_t len_priv, len_privx, len_rat, len_bpub, len_tx, len_rxs, len_rxf, other;
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
  struct sensei_secure s;
};

static void backend_session(struct backend *b, const uint8_t *node_pub) {
  struct sensei_secure_keys keys;
  sensei_secure_key_t own = 0;

  zassert_equal(sensei_secure_keygen(C, b->priv, b->pub), 0);
  zassert_equal(C->ecdh_import(C->ctx, b->priv, sizeof(b->priv), &own), 0);
  zassert_equal(sensei_secure_derive(C, own, SENSEI_SECURE_ROLE_BACKEND, node_pub, b->pub, &keys), 0);
  C->key_destroy(C->ctx, own);
  memset(&b->s, 0, sizeof(b->s));
  zassert_equal(sensei_secure_init(&b->s, C, SENSEI_SECURE_ROLE_BACKEND, &keys, 7, 0, 1, NULL, 0, NULL), 0);
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
    int n = sensei_e2e_seal_record(C, k, v->name, v->nonce, v->plain, v->plain_len, rec, sizeof(rec));

    zassert_equal(n, (int)v->record_len);
    zassert_mem_equal(rec, v->record, v->record_len);
    n = sensei_e2e_open_record(C, k, v->name, v->record, v->record_len, plain, sizeof(plain));
    zassert_equal(n, (int)v->plain_len);
    zassert_mem_equal(plain, v->plain, v->plain_len);
    zassert_equal(sensei_e2e_open_record(C, k, "e2e_other", v->record, v->record_len, plain, sizeof(plain)), -EACCES);
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
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE, "kept until the confirmation");
  zassert_equal(sensei_e2e_settings_confirm(), 0);
  zassert_equal(stored().len_priv, 0);
  stop(&e2);
  sensei_secure_close(&b.s);
}

ZTEST(secure_settings, test_the_name_e2e_rat_is_reserved) {
  struct sensei_e2e e;
  uint8_t pt[152] = {1}, rec[sizeof(pt) + SENSEI_E2E_SEAL_OVERHEAD];
  struct names n;

  boot(&e);
  zassert_equal(sensei_e2e_settings_seal("e2e_rat", pt, sizeof(pt), rec, sizeof(rec)), (int)sizeof(rec));
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", rec, sizeof(rec)), 0);
  stop(&e);
  boot(&e); /* a load does not look at it */
  n = stored();
  zassert_equal(n.len_rat, sizeof(rec));
  zassert_equal(n.other, 0);
  stop(&e);
  zassert_equal(sensei_e2e_settings_ops.erase(NULL), 0); /* erase and reset delete it */
  zassert_equal(stored().len_rat, 0);
  boot(&e);
  zassert_equal(settings_save_one(PREFIX "/e2e_rat", rec, sizeof(rec)), 0);
  zassert_equal(sensei_e2e_reset(&e), 0);
  zassert_equal(stored().len_rat, 0);
  stop(&e);
}

#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

ZTEST_SUITE(secure_settings, NULL, setup, before, NULL, NULL);
