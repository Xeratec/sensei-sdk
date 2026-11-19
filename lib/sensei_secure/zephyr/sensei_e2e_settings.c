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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_ON_OTA
#include "ota/sensei_ota.h"
#endif

LOG_MODULE_REGISTER(sensei_e2e_settings, CONFIG_LOG_DEFAULT_LEVEL);

#define PFX CONFIG_SENSEI_SECURE_SETTINGS_PREFIX

#define NAME_PRIV "e2e_priv"   /* plain private key (before sealing) */
#define NAME_PRIVX "e2e_privx" /* sealed private key */
#define NAME_RAT "e2e_rat"     /* ratchet state (sealed with the key id as context) */
#define NAME_MRK "e2e_mrk"     /* fingerprint of the sealed key: it was sealed by a firmware that knows this marker */
#define SEALED_PRIV_SIZE (SENSEI_SECURE_PRIV_SIZE + SENSEI_E2E_SEAL_OVERHEAD)
#define MRK_SIZE 4u
/* e2e_rat: with sealing key_id (4 B, plain, also the context of the seal) || sealed record, else the plain record. */
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
#define RAT_STORED_SIZE (4u + SENSEI_E2E_RAT_SIZE + SENSEI_E2E_SEAL_OVERHEAD)
#else
#define RAT_STORED_SIZE SENSEI_E2E_RAT_SIZE
#endif

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;

  while (n--) {
    *v++ = 0;
  }
}

/* A plain e2e_priv is still stored next to the sealed key and may be deleted (sensei_e2e_settings_confirm()). The flag
 * is read and written by the key store thread and by the thread that confirms the image: atomic. */
static atomic_t plain_pending_flag;

#define plain_pending_get() (atomic_get(&plain_pending_flag) != 0)
#define plain_pending_set(v) ((void)atomic_set(&plain_pending_flag, (v) ? 1 : 0))

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

int sensei_e2e_settings_seal_ctx(const char *name, const uint8_t *ctx, size_t ctx_len, const uint8_t *pt, size_t len,
                                 uint8_t *out, size_t cap) {
  int err = sensei_e2e_settings_seal_status();

  return err ? err
             : sensei_e2e_seal_record_ctx(sensei_secure_crypto_zephyr(), seal_key, name, ctx, ctx_len, NULL, pt, len,
                                          out, cap);
}

int sensei_e2e_settings_unseal_ctx(const char *name, const uint8_t *ctx, size_t ctx_len, const uint8_t *rec, size_t len,
                                   uint8_t *out, size_t cap) {
  int err = sensei_e2e_settings_seal_status();

  return err ? err
             : sensei_e2e_open_record_ctx(sensei_secure_crypto_zephyr(), seal_key, name, ctx, ctx_len, rec, len, out,
                                          cap);
}

int sensei_e2e_settings_seal(const char *name, const uint8_t *pt, size_t len, uint8_t *out, size_t cap) {
  return sensei_e2e_settings_seal_ctx(name, NULL, 0, pt, len, out, cap);
}

int sensei_e2e_settings_unseal(const char *name, const uint8_t *rec, size_t len, uint8_t *out, size_t cap) {
  return sensei_e2e_settings_unseal_ctx(name, NULL, 0, rec, len, out, cap);
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
int sensei_e2e_settings_seal_ctx(const char *name, const uint8_t *ctx, size_t ctx_len, const uint8_t *pt, size_t len,
                                 uint8_t *out, size_t cap) {
  ARG_UNUSED(name);
  ARG_UNUSED(ctx);
  ARG_UNUSED(ctx_len);
  ARG_UNUSED(pt);
  ARG_UNUSED(len);
  ARG_UNUSED(out);
  ARG_UNUSED(cap);
  return -ENOTSUP;
}
int sensei_e2e_settings_unseal_ctx(const char *name, const uint8_t *ctx, size_t ctx_len, const uint8_t *rec, size_t len,
                                   uint8_t *out, size_t cap) {
  ARG_UNUSED(name);
  ARG_UNUSED(ctx);
  ARG_UNUSED(ctx_len);
  ARG_UNUSED(rec);
  ARG_UNUSED(len);
  ARG_UNUSED(out);
  ARG_UNUSED(cap);
  return -ENOTSUP;
}

#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
/* e2e_mrk = 4 bytes of HKDF(priv, "SENSEI-E2E-mrk-v1"): the key that this firmware (or a later one) sealed last.
 * Firmware from before the marker (the first keys-at-rest image) does not write it. See resolve_priv(). */
static void key_fp(const uint8_t priv[SENSEI_SECURE_PRIV_SIZE], uint8_t fp[MRK_SIZE]) {
  static const uint8_t salt[] = "SENSEI-E2E-mrk-v1";
  const struct sensei_secure_crypto *c = sensei_secure_crypto_zephyr();

  if (c->hkdf(c->ctx, priv, SENSEI_SECURE_PRIV_SIZE, salt, sizeof(salt) - 1, NULL, 0, fp, MRK_SIZE) != 0) {
    memset(fp, 0, MRK_SIZE);
  }
}

/* A marker that names another key than the sealed one would make a plain key of a later downgrade look older than it
 * is. If the new marker cannot be written, the old one goes (no marker: the plain key is taken for the newer one). */
static void write_marker(const uint8_t priv[SENSEI_SECURE_PRIV_SIZE]) {
  uint8_t fp[MRK_SIZE];
  int err;

  key_fp(priv, fp);
  err = settings_save_one(PFX "/" NAME_MRK, fp, sizeof(fp));
  if (err) {
    LOG_WRN("key marker not written: %d", err);
    err = settings_delete(PFX "/" NAME_MRK);
    if (err) {
      LOG_ERR("old key marker not deleted: %d", err);
    }
  }
}
#endif

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
  /* The marker names the key that was sealed last: the old one goes before the new key is written, so that it never
   * names a key that is not the sealed one (a failed marker write below leaves none, not a stale one). */
  err = settings_delete(PFX "/" NAME_MRK);
  if (err) {
    LOG_ERR("old key marker not deleted: %d", err);
    return err;
  }
  err = settings_save_one(PFX "/" NAME_PRIVX, rec, (size_t)n);
  if (err == 0) {
    plain_pending_set(false);
    write_marker(priv);
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

/* The ratchet record: one settings entry, so one atomic write (NVS: an entry is valid or not, a power cut leaves the
 * old one). Sealed under the context key_id: a record of an old key pair does not open for the new one. */
static int save_rat(void *u, uint32_t key_id, const uint8_t *rec, size_t len) {
  ARG_UNUSED(u);
  uint8_t out[RAT_STORED_SIZE];
  int err;

  if (rec == NULL) {
    return settings_delete(PFX "/" NAME_RAT);
  }
  if (len != SENSEI_E2E_RAT_SIZE) {
    return -EINVAL;
  }
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
  out[0] = (uint8_t)key_id;
  out[1] = (uint8_t)(key_id >> 8);
  out[2] = (uint8_t)(key_id >> 16);
  out[3] = (uint8_t)(key_id >> 24);
  err = sensei_e2e_settings_seal_ctx(NAME_RAT, out, 4, rec, len, out + 4, sizeof(out) - 4);
  if (err < 0) {
    wipe(out, sizeof(out));
    return err;
  }
  err = settings_save_one(PFX "/" NAME_RAT, out, 4u + (size_t)err);
#else
  ARG_UNUSED(key_id);
  memcpy(out, rec, len);
  err = settings_save_one(PFX "/" NAME_RAT, out, sizeof(out));
#endif
  wipe(out, sizeof(out));
  return err;
}

/* The collector key first, then the ratchet record (it belongs to the key) and the key records, then the commissioning
 * records (a verifier does not outlive a reset: the old label must fail), the counters last: a counter must not come
 * back with a key that survived a failed delete. The first error stops it and is returned
 * (sensei_e2e_reset() then refuses). */
static int erase(void *u) {
  static const char *const names[] = {
      PFX "/e2e_bpub",  PFX "/" NAME_RAT, PFX "/" NAME_PRIV, PFX "/" NAME_PRIVX,
      PFX "/" NAME_MRK, PFX "/e2e_cmv",   PFX "/e2e_cmf", /* commissioning: the verifier and its attempt counters
                                                             (sensei_commission_settings.h) */
      PFX "/e2e_tx",    PFX "/e2e_rxs",   PFX "/e2e_rxf",    PFX "/e2e_rx" /* first version of the node firmware */
  };

  ARG_UNUSED(u);
  for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
    int err = settings_delete(names[i]);

    if (err) {
      LOG_ERR("erase: %s not deleted: %d", names[i], err);
      return err;
    }
  }
  plain_pending_set(false);
  return 0;
}

/* Clocks of the key store: uptime from the kernel, the wall clock from the application (weak: 0, not known). */
__weak uint32_t sensei_e2e_unix_time(void) { return 0; }

static uint32_t clock_up(void *u) {
  ARG_UNUSED(u);
  return (uint32_t)(k_uptime_get() / MSEC_PER_SEC);
}

static uint32_t clock_unix(void *u) {
  ARG_UNUSED(u);
  return sensei_e2e_unix_time();
}

const struct sensei_e2e_ops sensei_e2e_settings_ops = {
    .save_priv = save_priv,
    .save_backend = save_backend,
    .save_tx = save_tx,
    .save_rx = save_rx,
    .erase = erase,
    .save_floor = save_floor,
    .save_rat = save_rat,
    .uptime_s = clock_up,
    .unix_s = clock_unix,
};

/* The target of a load; NULL when the settings core loads at start and nobody asked for the state. */
struct load {
  struct sensei_e2e_nv *nv;
  uint8_t privx[SEALED_PRIV_SIZE]; /* the sealed key record */
  bool have_privx;                 /* a sealed record of the right size was read */
  bool bad_privx;                  /* a sealed record of another size */
  bool have_plain;                 /* the plain key was read into nv->priv */
  uint8_t rat[RAT_STORED_SIZE];    /* e2e_rat as stored */
  bool have_rat;                   /* a record of the right size was read */
  bool bad_rat;                    /* a record of another size */
  uint8_t mrk[MRK_SIZE];           /* e2e_mrk */
  bool have_mrk;
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
  } else if (settings_name_steq(name, NAME_RAT, &next) && !next) {
    if (len == sizeof(l->rat)) {
      l->have_rat = read_cb(cb_arg, l->rat, len) == (ssize_t)len;
      l->bad_rat = !l->have_rat;
    } else {
      l->bad_rat = true;
    }
  } else if (settings_name_steq(name, NAME_MRK, &next) && !next && len == MRK_SIZE) {
    l->have_mrk = read_cb(cb_arg, l->mrk, len) == (ssize_t)len;
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

  if (err == 0) { /* the old marker goes first (see save_priv()) */
    err = settings_delete(PFX "/" NAME_MRK);
    if (err) {
      LOG_ERR("old key marker not deleted: %d", err);
    }
  }
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
    plain_pending_set(true);
    write_marker(nv->priv);
    LOG_INF("private key sealed (e2e_privx); the plain record stays until the image is confirmed");
  }
  return err;
}

/* The marker that was read names this key. */
static bool marker_matches(const struct load *l, const uint8_t priv[SENSEI_SECURE_PRIV_SIZE]) {
  uint8_t fp[MRK_SIZE];

  key_fp(priv, fp);
  return l->have_mrk && memcmp(l->mrk, fp, MRK_SIZE) == 0;
}

/* Two stored keys, the plain one valid and different from the sealed one: which one does the ratchet record belong to?
 * The record starts with its key id; the key id of a key is derived with the collector key. A key whose id is the one
 * in the record is the key that talked last. 1: the plain key, -1: the sealed key, 0: the record tells nothing (none,
 * no collector key, both or none match). */
static int rat_tiebreak(const struct load *l, const struct sensei_e2e_nv *nv,
                        const uint8_t sealed[SENSEI_SECURE_PRIV_SIZE]) {
  const struct sensei_secure_crypto *c = sensei_secure_crypto_zephyr();
  uint32_t rec_id, id_plain, id_sealed;

  if (!l->have_rat || !nv->have_backend) {
    return 0;
  }
  rec_id = (uint32_t)l->rat[0] | ((uint32_t)l->rat[1] << 8) | ((uint32_t)l->rat[2] << 16) | ((uint32_t)l->rat[3] << 24);
  if (sensei_e2e_key_id_of(c, nv->priv, nv->backend_pub, &id_plain) != 0 ||
      sensei_e2e_key_id_of(c, sealed, nv->backend_pub, &id_sealed) != 0 || id_plain == id_sealed) {
    return 0;
  }
  return rec_id == id_plain ? 1 : (rec_id == id_sealed ? -1 : 0);
}

/* Settle the private key of the node: open the sealed record, or migrate the plain one. */
static int resolve_priv(struct load *l) {
  struct sensei_e2e_nv *nv = l->nv;
  int n;

  plain_pending_set(false);
  if (l->have_privx || l->bad_privx) {
    uint8_t key[SENSEI_SECURE_PRIV_SIZE];

    n = l->have_privx ? sensei_e2e_settings_unseal(NAME_PRIVX, l->privx, sizeof(l->privx), key, sizeof(key)) : -EBADMSG;
    if (n == (int)sizeof(key)) {
      if (l->have_plain && memcmp(key, nv->priv, sizeof(key)) != 0) {
        bool plain_ok = sensei_secure_priv_valid(nv->priv);
        int tb = plain_ok ? rat_tiebreak(l, nv, key) : 0;

        if (!plain_ok) { /* the plain record is damaged, not a key: the sealed one stands */
          LOG_WRN("plain key record is not a valid key, the sealed key stands");
          memcpy(nv->priv, key, sizeof(key));
          wipe(key, sizeof(key));
          plain_pending_set(true);
        } else if (tb < 0 || (tb == 0 && l->have_mrk && !marker_matches(l, key))) {
          /* The ratchet record belongs to the sealed key, or (no help from it) the marker names another key than the
           * sealed one: the sealed key was written after this firmware last sealed, by one that does not write the
           * marker (the first keys-at-rest image seals first and ignores a failed delete). The plain key is the older
           * one: the sealed key stands and the plain record goes now. */
          LOG_WRN("plain key next to a newer sealed key: the sealed key stands");
          n = settings_delete(PFX "/" NAME_PRIV);
          memcpy(nv->priv, key, sizeof(key));
          wipe(key, sizeof(key));
          if (n == 0) {
            write_marker(nv->priv);
          } else {
            LOG_ERR("plain key record not deleted: %d", n);
            plain_pending_set(true);
          }
        } else {
          /* Both records, different keys, and the ratchet record belongs to the plain key, or it tells nothing and the
           * marker names the sealed key (or there is none): an older firmware made a new plain key after the key was
           * sealed (downgrade, then upgrade). This firmware deletes the plain record before it writes a new sealed key,
           * so a plain key next to its own sealed key is the newer one: the collector was provisioned for it. Seal it
           * again. Without a marker and without a record the sealed key may come from the first keys-at-rest image,
           * which can leave an older plain key behind: this case is not told apart. */
          LOG_WRN("plain key next to a different sealed key: the plain key is newer, sealing it again");
          wipe(key, sizeof(key));
          n = migrate(l);
          if (n != 0) {
            wipe(nv->priv, sizeof(nv->priv));
            nv->key_unreadable = true;
            return n;
          }
        }
      } else {
        plain_pending_set(l->have_plain); /* the same key twice: the migration of an image that is not confirmed yet */
        memcpy(nv->priv, key, sizeof(key));
        wipe(key, sizeof(key));
        if (!marker_matches(l, nv->priv)) {
          write_marker(nv->priv); /* from now on a different plain key is a newer one */
        }
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

  if (!plain_pending_get()) {
    return 0;
  }
  err = settings_delete(PFX "/" NAME_PRIV);
  if (err) {
    LOG_ERR("plain key record not deleted: %d (it stays until the next try)", err);
    return err;
  }
  plain_pending_set(false);
  LOG_INF("plain key record deleted");
  return 0;
}

bool sensei_e2e_settings_plain_pending(void) { return plain_pending_get(); }

#else

static int resolve_priv(struct load *l) {
  plain_pending_set(false);
  l->nv->have_priv = l->have_plain; /* a sealed record is ignored without sealing */
  return 0;
}

int sensei_e2e_settings_confirm(void) { return 0; }

bool sensei_e2e_settings_plain_pending(void) { return false; }

#endif /* CONFIG_SENSEI_SECURE_SETTINGS_SEAL */

/* The ratchet record: opened with its key id as context. A record that does not open, or has another size, is lost
 * state (sensei_e2e_init() then refuses to talk until the node is provisioned again). */
static void resolve_rat(struct load *l) {
  struct sensei_e2e_nv *nv = l->nv;

  if (l->bad_rat) {
    nv->rat_unreadable = true;
    LOG_ERR("ratchet record has the wrong size");
    return;
  }
  if (!l->have_rat) {
    return;
  }
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
  {
    int n =
        sensei_e2e_settings_unseal_ctx(NAME_RAT, l->rat, 4, l->rat + 4, sizeof(l->rat) - 4, nv->rat, sizeof(nv->rat));

    if (n == (int)SENSEI_E2E_RAT_SIZE) {
      nv->have_rat = true;
    } else {
      nv->rat_unreadable = true;
      LOG_ERR("ratchet record does not open: %d", n);
    }
  }
#else
  memcpy(nv->rat, l->rat, sizeof(nv->rat));
  nv->have_rat = true;
#endif
}

#if defined(CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_ON_OTA)
/* The OTA module calls the hook from the system work queue (or the thread that confirms). The delete can block for the
 * time of a garbage collection of the settings (NVS erases a sector), so it runs on a work queue of its own. */
static K_THREAD_STACK_DEFINE(confirm_stack, CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_STACK_SIZE);
static struct k_work_q confirm_q;
static struct k_work confirm_work;

static void confirm_work_fn(struct k_work *work) {
  ARG_UNUSED(work);
  (void)sensei_e2e_settings_confirm();
}

static void ota_confirmed(void *user) {
  ARG_UNUSED(user);
  (void)k_work_submit_to_queue(&confirm_q, &confirm_work);
}
#endif

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
  if (err == 0) {
    resolve_rat(&l);
  }
  wipe(l.privx, sizeof(l.privx)); /* sealed, not secret, but nothing needs to stay */
  wipe(l.rat, sizeof(l.rat));
  if (err == 0) {
#if defined(CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_ON_OTA)
    /* The OTA module calls this when the running image is confirmed (also at once if it is already), whatever the
     * way: the node, an SMP confirm of the host, a start of a confirmed image. */
    static bool queue_started, registered;

    if (!queue_started) {
      k_work_queue_init(&confirm_q);
      k_work_queue_start(&confirm_q, confirm_stack, K_THREAD_STACK_SIZEOF(confirm_stack),
                         K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
      k_thread_name_set(&confirm_q.thread, "sensei_e2e_confirm");
      k_work_init(&confirm_work, confirm_work_fn);
      queue_started = true;
    }
    if (!registered) {
      registered = sensei_ota_on_confirmed(ota_confirmed, NULL) == 0;
    }
#endif
#if defined(CONFIG_SENSEI_SECURE_SETTINGS_CONFIRM_AFTER_LOAD)
    /* No bootloader that can revert the image: the old image cannot come back, nothing to wait for. */
    (void)sensei_e2e_settings_confirm();
#endif
  }
  return err;
}
