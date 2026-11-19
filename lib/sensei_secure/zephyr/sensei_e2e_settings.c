/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_e2e_settings.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(sensei_e2e_settings, CONFIG_LOG_DEFAULT_LEVEL);

#define PFX CONFIG_SENSEI_SECURE_SETTINGS_PREFIX

#define NAME_PRIV "e2e_priv"   /* plain private key (before sealing) */
#define NAME_PRIVX "e2e_privx" /* sealed private key */
#define NAME_RAT "e2e_rat"     /* reserved: ratchet state of a later version (sealed) */
#define SEALED_PRIV_SIZE (SENSEI_SECURE_PRIV_SIZE + SENSEI_E2E_SEAL_OVERHEAD)

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;

  while (n--) {
    *v++ = 0;
  }
}

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL

/* The sealing key is held as a volatile PSA key; one thread uses it (the one of the key store). */
static sensei_secure_key_t seal_key;
static int seal_err = -ENODATA; /* -ENODATA: the provider was not asked yet */

bool sensei_e2e_settings_sealed(void) { return true; }

int sensei_e2e_settings_seal_status(void) {
  const struct sensei_secure_crypto *c;
  uint8_t k[SENSEI_SECURE_KEY_SIZE];
  int err;

  if (seal_err != -ENODATA) {
    return seal_err; /* a key, or a failure that stays until seal_close() */
  }
  c = sensei_secure_crypto_zephyr();
  err = sensei_e2e_settings_seal_key(k);
  if (err == 0 && c->aead_import(c->ctx, k, &seal_key) != 0) {
    seal_key = 0;
    err = -EIO;
  }
  wipe(k, sizeof(k));
  if (err > 0) {
    err = -EIO;
  }
  seal_err = err;
  if (err) {
    LOG_ERR("sealing key not available: %d", err);
  }
  return err;
}

void sensei_e2e_settings_seal_close(void) {
  if (seal_key) {
    const struct sensei_secure_crypto *c = sensei_secure_crypto_zephyr();

    (void)c->key_destroy(c->ctx, seal_key);
    seal_key = 0;
  }
  seal_err = -ENODATA;
}

int sensei_e2e_settings_seal(const char *name, const uint8_t *pt, size_t len, uint8_t *out, size_t cap) {
  int err = sensei_e2e_settings_seal_status();

  return err ? err : sensei_e2e_seal_record(sensei_secure_crypto_zephyr(), seal_key, name, NULL, pt, len, out, cap);
}

int sensei_e2e_settings_unseal(const char *name, const uint8_t *rec, size_t len, uint8_t *out, size_t cap) {
  int err = sensei_e2e_settings_seal_status();

  return err ? err : sensei_e2e_open_record(sensei_secure_crypto_zephyr(), seal_key, name, rec, len, out, cap);
}

#else

bool sensei_e2e_settings_sealed(void) { return false; }
int sensei_e2e_settings_seal_status(void) { return -ENOTSUP; }
void sensei_e2e_settings_seal_close(void) {}
int sensei_e2e_settings_seal(const char *name, const uint8_t *pt, size_t len, uint8_t *out, size_t cap) {
  ARG_UNUSED(name);
  ARG_UNUSED(pt);
  ARG_UNUSED(len);
  ARG_UNUSED(out);
  ARG_UNUSED(cap);
  return -ENOTSUP;
}
int sensei_e2e_settings_unseal(const char *name, const uint8_t *rec, size_t len, uint8_t *out, size_t cap) {
  ARG_UNUSED(name);
  ARG_UNUSED(rec);
  ARG_UNUSED(len);
  ARG_UNUSED(out);
  ARG_UNUSED(cap);
  return -ENOTSUP;
}

#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

static int save_priv(void *u, const uint8_t *priv) {
  ARG_UNUSED(u);
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
  uint8_t rec[SEALED_PRIV_SIZE];
  int n = sensei_e2e_settings_seal(NAME_PRIVX, priv, SENSEI_SECURE_PRIV_SIZE, rec, sizeof(rec));
  int err;

  if (n < 0) {
    return n;
  }
  err = settings_save_one(PFX "/" NAME_PRIVX, rec, (size_t)n);
  if (err == 0) {
    (void)settings_delete(PFX "/" NAME_PRIV); /* no plain copy next to the sealed one */
  }
  return err;
#else
  return settings_save_one(PFX "/" NAME_PRIV, priv, SENSEI_SECURE_PRIV_SIZE);
#endif
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
  (void)settings_delete(PFX "/" NAME_PRIV);
  (void)settings_delete(PFX "/" NAME_PRIVX);
  (void)settings_delete(PFX "/" NAME_RAT);
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
struct load {
  struct sensei_e2e_nv *nv;
  uint8_t privx[SEALED_PRIV_SIZE]; /* the sealed key record */
  bool have_privx;                 /* a sealed record of the right size was read */
  bool bad_privx;                  /* a sealed record of another size */
  bool have_plain;                 /* the plain key was read into nv->priv */
};

static struct load *loading;

static int set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
  const char *next;
  struct load *l = loading;
  struct sensei_e2e_nv *nv;

  if (l == NULL) {
    return 0;
  }
  nv = l->nv;
  if (settings_name_steq(name, NAME_PRIV, &next) && !next && len == SENSEI_SECURE_PRIV_SIZE) {
    l->have_plain = read_cb(cb_arg, nv->priv, len) == (ssize_t)len;
  } else if (settings_name_steq(name, NAME_PRIVX, &next) && !next) {
    if (len == sizeof(l->privx)) {
      l->have_privx = read_cb(cb_arg, l->privx, len) == (ssize_t)len;
    } else {
      l->bad_privx = true;
    }
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
  /* Other names (e2e_rx and the symmetric key of earlier versions, the reserved e2e_rat) are ignored. */
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sensei_e2e, CONFIG_SENSEI_SECURE_SETTINGS_PREFIX, NULL, set_cb, NULL, NULL);

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL

/* Read back the sealed key record that was just written. */
static int readback_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct load *l = param;

  if (!strcmp(key, NAME_PRIVX) && len == sizeof(l->privx)) {
    l->have_privx = read_cb(cb_arg, l->privx, len) == (ssize_t)len;
  }
  return 0;
}

/* The plain key of an earlier version is in nv->priv: seal it, store it, read it back, compare, delete the plain one.
 */
static int migrate(struct load *l) {
  const struct sensei_e2e_nv *nv = l->nv;
  uint8_t rec[SEALED_PRIV_SIZE], back[SENSEI_SECURE_PRIV_SIZE] = {0};
  int n = sensei_e2e_settings_seal(NAME_PRIVX, nv->priv, SENSEI_SECURE_PRIV_SIZE, rec, sizeof(rec));
  int err = n < 0 ? n : 0;
  bool written = false;

  if (err == 0) {
    err = settings_save_one(PFX "/" NAME_PRIVX, rec, (size_t)n);
    written = err == 0;
  }
  if (err == 0) {
    l->have_privx = false;
    err = settings_load_subtree_direct(PFX, readback_cb, l);
    if (err == 0) {
      n = l->have_privx ? sensei_e2e_settings_unseal(NAME_PRIVX, l->privx, sizeof(l->privx), back, sizeof(back))
                        : -ENOENT;
      err = n == (int)sizeof(back) && !memcmp(back, nv->priv, sizeof(back)) ? 0 : (n < 0 ? n : -EIO);
    }
  }
  wipe(back, sizeof(back));
  if (err != 0 && written) {
    (void)settings_delete(PFX "/" NAME_PRIVX); /* a record that did not check out must not shadow the plain key */
  }
  if (err == 0) {
    err = settings_delete(PFX "/" NAME_PRIV); /* if this fails the sealed record stays: the next start finishes */
  }
  if (err) {
    LOG_ERR("key migration failed: %d (the plain key stays)", err);
  } else {
    LOG_INF("private key sealed (e2e_privx), plain record deleted");
  }
  return err;
}

/* Settle the private key of the node: open the sealed record, or migrate the plain one. */
static int resolve_priv(struct load *l) {
  struct sensei_e2e_nv *nv = l->nv;
  int n;

  if (l->have_privx || l->bad_privx) {
    uint8_t key[SENSEI_SECURE_PRIV_SIZE];

    n = l->have_privx ? sensei_e2e_settings_unseal(NAME_PRIVX, l->privx, sizeof(l->privx), key, sizeof(key)) : -EBADMSG;
    if (n == (int)sizeof(key)) {
      memcpy(nv->priv, key, sizeof(key));
      nv->have_priv = true;
      if (l->have_plain) { /* the migration stopped between the two writes */
        (void)settings_delete(PFX "/" NAME_PRIV);
      }
      wipe(key, sizeof(key));
      return 0;
    }
    wipe(key, sizeof(key));
    wipe(nv->priv, sizeof(nv->priv));
    nv->key_unreadable = true;
    LOG_ERR("sealed key record does not open: %d", n);
    return n < 0 ? n : -EIO;
  }
  if (l->have_plain) {
    n = migrate(l);
    if (n == 0) {
      nv->have_priv = true;
      return 0;
    }
    wipe(nv->priv, sizeof(nv->priv));
    nv->key_unreadable = true;
    return n;
  }
  return 0; /* no key stored: the key store makes one and saves it sealed */
}

#else

static int resolve_priv(struct load *l) {
  l->nv->have_priv = l->have_plain; /* a sealed record is ignored without sealing */
  return 0;
}

#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

int sensei_e2e_settings_load(struct sensei_e2e_nv *nv) {
  struct load l;
  int err;

  memset(nv, 0, sizeof(*nv));
  memset(&l, 0, sizeof(l));
  l.nv = nv;
  loading = &l;
  err = settings_load_subtree(CONFIG_SENSEI_SECURE_SETTINGS_PREFIX);
  loading = NULL;
  if (err == 0) {
    err = resolve_priv(&l);
  }
  wipe(l.privx, sizeof(l.privx)); /* sealed, not secret, but nothing needs to stay */
  return err;
}
