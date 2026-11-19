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

/* A plain e2e_priv is still stored next to the sealed key and may be deleted (sensei_e2e_settings_confirm()). */
static bool plain_pending;

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

/* Providers that need nothing before the first use keep this; the HUK provider replaces it. */
__weak int sensei_e2e_settings_seal_prepare(void) { return 0; }

int sensei_e2e_settings_reset_check(void) {
  sensei_e2e_settings_seal_close(); /* a failure that was cached at the start must not hide a provider that works now */
  return sensei_e2e_settings_seal_status();
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
int sensei_e2e_settings_seal_prepare(void) { return 0; }
int sensei_e2e_settings_reset_check(void) { return 0; }
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
  /* A new key: an older plain key is not its twin and must not stay (a later start would take it for the newer one).
   * It goes first: if the delete fails nothing was written and the caller keeps no key. */
  err = settings_delete(PFX "/" NAME_PRIV);
  if (err) {
    LOG_ERR("plain key record not deleted: %d", err);
    return err;
  }
  err = settings_save_one(PFX "/" NAME_PRIVX, rec, (size_t)n);
  if (err == 0) {
    plain_pending = false;
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

/* The collector key first, then the key records, the counters last: a counter must not come back with a key that
 * survived a failed delete. The first error stops it and is returned (sensei_e2e_reset() then refuses). */
static int erase(void *u) {
  static const char *const names[] = {
      PFX "/e2e_bpub", PFX "/" NAME_PRIV, PFX "/" NAME_PRIVX, PFX "/" NAME_RAT,
      PFX "/e2e_tx",   PFX "/e2e_rxs",    PFX "/e2e_rxf",     PFX "/e2e_rx" /* first version of the node firmware */
  };

  ARG_UNUSED(u);
  for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
    int err = settings_delete(names[i]);

    if (err) {
      LOG_ERR("erase: %s not deleted: %d", names[i], err);
      return err;
    }
  }
  plain_pending = false;
  return 0;
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

  if (key != NULL && !strcmp(key, NAME_PRIVX) && len == sizeof(l->privx)) {
    l->have_privx = read_cb(cb_arg, l->privx, len) == (ssize_t)len;
  }
  return 0;
}

/* The plain key of an earlier version is in nv->priv: seal it, store it, read it back, compare. The plain record stays:
 * the old image may still be started again (MCUboot reverts a trial image that is not confirmed). It is deleted by
 * sensei_e2e_settings_confirm(). */
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
    int derr = settings_delete(PFX "/" NAME_PRIVX); /* a record that did not check out must not shadow the plain key */

    if (derr) {
      LOG_ERR("bad sealed record not deleted: %d", derr);
    }
  }
  if (err) {
    LOG_ERR("key migration failed: %d (the plain key stays)", err);
  } else {
    plain_pending = true;
    LOG_INF("private key sealed (e2e_privx); the plain record stays until the image is confirmed");
  }
  return err;
}

/* Settle the private key of the node: open the sealed record, or migrate the plain one. */
static int resolve_priv(struct load *l) {
  struct sensei_e2e_nv *nv = l->nv;
  int n;

  plain_pending = false;
  if (l->have_privx || l->bad_privx) {
    uint8_t key[SENSEI_SECURE_PRIV_SIZE];

    n = l->have_privx ? sensei_e2e_settings_unseal(NAME_PRIVX, l->privx, sizeof(l->privx), key, sizeof(key)) : -EBADMSG;
    if (n == (int)sizeof(key)) {
      if (l->have_plain && memcmp(key, nv->priv, sizeof(key)) != 0) {
        if (sensei_secure_priv_valid(nv->priv)) {
          /* Both records, different keys: an older firmware made a new plain key after the key was sealed (downgrade,
           * then upgrade). The plain one is the newer: the collector was provisioned for it. Seal it again. */
          LOG_WRN("plain key next to a different sealed key: the plain key is newer, sealing it again");
          wipe(key, sizeof(key));
          n = migrate(l);
          if (n != 0) {
            wipe(nv->priv, sizeof(nv->priv));
            nv->key_unreadable = true;
            return n;
          }
        } else { /* the plain record is damaged, not a key: the sealed one stands */
          LOG_WRN("plain key record is not a valid key, the sealed key stands");
          memcpy(nv->priv, key, sizeof(key));
          wipe(key, sizeof(key));
          plain_pending = true;
        }
      } else {
        plain_pending = l->have_plain; /* the same key twice: the migration of an image that is not confirmed yet */
        memcpy(nv->priv, key, sizeof(key));
        wipe(key, sizeof(key));
      }
      nv->have_priv = true;
      nv->priv_sealed = true;
      return 0;
    }
    wipe(key, sizeof(key));
    wipe(nv->priv, sizeof(nv->priv));
    nv->key_unreadable = true;
    LOG_ERR("sealed key record does not open: %d", n);
    return n < 0 ? n : -EIO;
  }
  if (l->have_plain) {
    if (!sensei_secure_priv_valid(nv->priv)) {
      /* Damaged, not a key: do not seal it. sensei_e2e_init() replaces it (priv_sealed stays false) and the new key
       * is sealed and stored; the plain record goes with that. */
      LOG_WRN("plain key record is not a valid key: it is replaced");
      nv->have_priv = true;
      return 0;
    }
    n = migrate(l);
    if (n == 0) {
      nv->have_priv = true;
      nv->priv_sealed = true;
      return 0;
    }
    wipe(nv->priv, sizeof(nv->priv));
    nv->key_unreadable = true;
    return n;
  }
  return 0; /* no key stored: the key store makes one and saves it sealed */
}

int sensei_e2e_settings_confirm(void) {
  int err;

  if (!plain_pending) {
    return 0;
  }
  err = settings_delete(PFX "/" NAME_PRIV);
  if (err) {
    LOG_ERR("plain key record not deleted: %d (it stays until the next try)", err);
    return err;
  }
  plain_pending = false;
  LOG_INF("plain key record deleted");
  return 0;
}

bool sensei_e2e_settings_plain_pending(void) { return plain_pending; }

#else

static int resolve_priv(struct load *l) {
  plain_pending = false;
  l->nv->have_priv = l->have_plain; /* a sealed record is ignored without sealing */
  return 0;
}

int sensei_e2e_settings_confirm(void) { return 0; }

bool sensei_e2e_settings_plain_pending(void) { return false; }

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
