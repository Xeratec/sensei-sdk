/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* sensei_e2e on the settings subsystem (flash simulator of native_sim): the stored key names and sizes, the state
 * after a restart (the keys, the counters, the table of senders) and the erase. The logic itself is tested in
 * lib/sensei_secure/tests/test_e2e.c with a fake flash. */

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "sensei_e2e_settings.h"

#define PREFIX "sensei_comm" /* default of CONFIG_SENSEI_SECURE_SETTINGS_PREFIX: the keys of the SENSEI IEQ node */
#define STEP 4

static const struct sensei_secure_crypto *C;

struct names {
  size_t len_priv, len_bpub, len_tx, len_rxs, len_rxf, other;
};

static int list_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct names *n = param;

  ARG_UNUSED(read_cb);
  ARG_UNUSED(cb_arg);
  if (!strcmp(key, "e2e_priv")) {
    n->len_priv = len;
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

static void before(void *fixture) {
  ARG_UNUSED(fixture);
  sensei_e2e_settings_ops.erase(NULL); /* every test starts from an empty flash */
}

static void *setup(void) {
  C = sensei_secure_crypto_zephyr();
  zassert_not_null(C);
  zassert_equal(settings_subsys_init(), 0);
  return NULL;
}

/* "Reboot": read the settings and start the key store again. */
static void boot(struct sensei_e2e *e) {
  struct sensei_e2e_nv nv;

  zassert_equal(sensei_e2e_settings_load(&nv), 0);
  zassert_equal(sensei_e2e_init(e, C, &sensei_e2e_settings_ops, NULL, &nv, STEP), 0);
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
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE);
  zassert_equal(n.len_bpub + n.len_tx + n.len_rxs + n.len_rxf + n.other, 0);

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
  zassert_equal(n.len_priv, SENSEI_SECURE_PRIV_SIZE);
  zassert_equal(n.len_bpub + n.len_tx + n.len_rxs + n.len_rxf + n.other, 0);
  stop(&e);
  sensei_secure_close(&b.s);
}

ZTEST_SUITE(secure_settings, NULL, setup, before, NULL, NULL);
