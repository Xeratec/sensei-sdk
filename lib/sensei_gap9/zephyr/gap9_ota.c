/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/*
 * Update of the GAP9 application: the Zephyr side of g9ota_flow.c. The flow runs on the work queue of the
 * configuration (the host requests, the callbacks of the GAP9 link and the housekeeping timer all arrive there).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "crypto/sensei_crypto.h"
#include "sensei_gap9/g9ota_flow.h"
#include "sensei_gap9/gap9_ota_zephyr.h"
#include "sensei_gap9/gap9_zephyr.h"

LOG_MODULE_REGISTER(sensei_gap9_ota, CONFIG_SENSEI_GAP9_LOG_LEVEL);

#define SETTINGS_KEY "sensei_gap9/trial"
#define STORE_VERSION 2u

static const struct sensei_gap9_ota_cfg *cfg;
static struct g9ota_flow flow;
static struct k_work_delayable poll_work;

/* The stored trial state. */
static struct {
  uint8_t version;
  struct g9ota_store store;
} stored;
static bool stored_valid;

static int store_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "trial") == 0 && len == sizeof(stored) &&
      read_cb(cb_arg, &stored, sizeof(stored)) == (ssize_t)sizeof(stored) && stored.version == STORE_VERSION &&
      stored.store.trial.confirmed < 2 && stored.store.trial.trial < 2) {
    stored_valid = true;
  }
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sensei_gap9, "sensei_gap9", NULL, store_set, NULL, NULL);

/* ---- the world of the flow ---- */

static int op_job(const struct g9ota_req *req, g9ota_job_cb cb, void *user) { return cfg->job(req, cb, user); }

static int op_verify(const uint8_t hash[32], const uint8_t sig[64], const uint8_t *key_hash, size_t key_hash_len) {
  sensei_crypto_key_id_t key;
  sensei_crypto_status_t st;

  if (key_hash != NULL && (key_hash_len != cfg->key_hash_len || memcmp(key_hash, cfg->key_hash, key_hash_len) != 0)) {
    return -EACCES; /* signed with another key */
  }
  if (sensei_crypto_init() != SENSEI_CRYPTO_OK) {
    return -EIO;
  }
  if (sensei_crypto_ecdsa_import_public_key(cfg->pubkey, cfg->pubkey_len, &key) != SENSEI_CRYPTO_OK) {
    return -EIO;
  }
  st = sensei_crypto_ecdsa_verify_hash(key, hash, 32, sig, 64);
  (void)sensei_crypto_destroy_key(key);
  return st == SENSEI_CRYPTO_OK ? 0 : -EBADMSG;
}

static int op_persist(const struct g9ota_store *st) {
  stored.version = STORE_VERSION;
  stored.store = *st;
  return settings_save_one(SETTINGS_KEY, &stored, sizeof(stored)) < 0 ? -EIO : 0;
}

static uint32_t op_now(void) { return k_uptime_get_32(); }

static int op_trial_check(g9ota_check_cb cb, void *user) {
  if (cfg->trial_check == NULL) { /* no checks of the app: the status job was enough */
    cb(0, user);
    return 0;
  }
  return cfg->trial_check(cb, user);
}

/* Housekeeping: only while a session, a trial or a probe is going on. */
static void op_wake(void) { k_work_reschedule_for_queue(cfg->wq, &poll_work, K_SECONDS(5)); }

/* The link functions have the signatures of the flow. */
static const struct g9ota_ops ops = {
    .job = op_job,
    .hold = gap9_link_hold,
    .reboot = gap9_link_reboot,
    .set_slot = gap9_link_set_slot,
    .verify = op_verify,
    .persist = op_persist,
    .now_ms = op_now,
    .trial_check = op_trial_check,
    .wake = op_wake,
};

static void poll_fn(struct k_work *w) {
  ARG_UNUSED(w);
  g9ota_flow_poll(&flow);
  if (g9ota_flow_needs_poll(&flow)) {
    op_wake();
  }
}

int sensei_gap9_ota_init(const struct sensei_gap9_ota_cfg *c) {
  if (c == NULL || c->wq == NULL || c->job == NULL || c->pubkey == NULL || c->key_hash == NULL) {
    return -EINVAL;
  }
  cfg = c;
  k_work_init_delayable(&poll_work, poll_fn);
  (void)settings_load_subtree("sensei_gap9");
  if (g9ota_flow_init(&flow, &ops, stored_valid ? &stored.store : NULL) != 0) {
    return -EINVAL;
  }
  LOG_DBG("GAP9 slot %c", 'A' + g9ota_flow_slot(&flow)); /* debug only: the Status message has the slot */
  if (g9ota_flow_needs_poll(&flow)) {
    op_wake();
  }
  return 0;
}

struct g9ota_flow *sensei_gap9_ota_flow(void) { return &flow; }

uint8_t sensei_gap9_ota_slot(void) { return g9ota_flow_slot(&flow); }

uint32_t sensei_gap9_ota_image_id(void) { return flow.image_id; }
