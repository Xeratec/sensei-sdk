/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_e2e_settings.h"

#include <string.h>

#include <zephyr/settings/settings.h>

#define PFX CONFIG_SENSEI_SECURE_SETTINGS_PREFIX

static int save_priv(void *u, const uint8_t *priv) {
  ARG_UNUSED(u);
  return settings_save_one(PFX "/e2e_priv", priv, SENSEI_SECURE_PRIV_SIZE);
}

static int save_backend(void *u, const uint8_t *pub) {
  ARG_UNUSED(u);
  return settings_save_one(PFX "/e2e_bpub", pub, SENSEI_SECURE_PUB_SIZE);
}

static int save_tx(void *u, uint32_t r) {
  ARG_UNUSED(u);
  return settings_save_one(PFX "/e2e_tx", &r, sizeof(r));
}

static int save_rx(void *u, const struct sensei_secure_rx_state *tab, size_t n) {
  ARG_UNUSED(u);
  return settings_save_one(PFX "/e2e_rxs", tab, n * sizeof(*tab));
}

static int save_floor(void *u, uint32_t f) {
  ARG_UNUSED(u);
  return settings_save_one(PFX "/e2e_rxf", &f, sizeof(f));
}

static void erase(void *u) {
  ARG_UNUSED(u);
  (void)settings_delete(PFX "/e2e_priv");
  (void)settings_delete(PFX "/e2e_bpub");
  (void)settings_delete(PFX "/e2e_tx");
  (void)settings_delete(PFX "/e2e_rxs");
  (void)settings_delete(PFX "/e2e_rxf");
  (void)settings_delete(PFX "/e2e_rx"); /* first version of the node firmware */
}

const struct sensei_e2e_ops sensei_e2e_settings_ops = {
    .save_priv = save_priv,
    .save_backend = save_backend,
    .save_tx = save_tx,
    .save_rx = save_rx,
    .erase = erase,
    .save_floor = save_floor,
};

/* The target of a load; NULL when the settings core loads at start and nobody asked for the state. */
static struct sensei_e2e_nv *loading;

static int set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
  const char *next;
  struct sensei_e2e_nv *nv = loading;

  if (nv == NULL) {
    return 0;
  }
  if (settings_name_steq(name, "e2e_priv", &next) && !next && len == SENSEI_SECURE_PRIV_SIZE) {
    nv->have_priv = read_cb(cb_arg, nv->priv, len) == (ssize_t)len;
  } else if (settings_name_steq(name, "e2e_bpub", &next) && !next && len == SENSEI_SECURE_PUB_SIZE) {
    nv->have_backend = read_cb(cb_arg, nv->backend_pub, len) == (ssize_t)len;
  } else if (settings_name_steq(name, "e2e_tx", &next) && !next && len == sizeof(uint32_t)) {
    (void)read_cb(cb_arg, &nv->tx_reserved, len);
  } else if (settings_name_steq(name, "e2e_rxf", &next) && !next && len == sizeof(uint32_t)) {
    (void)read_cb(cb_arg, &nv->rx_floor, len);
  } else if (settings_name_steq(name, "e2e_rxs", &next) && !next && len % sizeof(nv->rx[0]) == 0 &&
             len <= sizeof(nv->rx)) {
    if (read_cb(cb_arg, nv->rx, len) == (ssize_t)len) {
      nv->n_rx = len / sizeof(nv->rx[0]);
    }
  }
  /* Other names (e2e_rx and the symmetric key of earlier versions) are ignored. */
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sensei_e2e, CONFIG_SENSEI_SECURE_SETTINGS_PREFIX, NULL, set_cb, NULL, NULL);

int sensei_e2e_settings_load(struct sensei_e2e_nv *nv) {
  int err;

  memset(nv, 0, sizeof(*nv));
  loading = nv;
  err = settings_load_subtree(CONFIG_SENSEI_SECURE_SETTINGS_PREFIX);
  loading = NULL;
  return err;
}
