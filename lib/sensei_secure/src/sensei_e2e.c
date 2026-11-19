/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_e2e.h"

#include <errno.h>
#include <string.h>

int sensei_e2e_errno(int err) {
  switch (err) {
  case 0:
    return 0;
  case SENSEI_SECURE_ERR_FORMAT:
    return -EBADMSG;
  case SENSEI_SECURE_ERR_KEY_ID:
  case SENSEI_SECURE_ERR_GEN: /* no key for the generation of the envelope */
  case SENSEI_SECURE_ERR_AUTH:
  case SENSEI_SECURE_ERR_NODE:
  case SENSEI_SECURE_ERR_KIND:
    return -EACCES;
  case SENSEI_SECURE_ERR_REPLAY:
    return -EALREADY;
  case SENSEI_SECURE_ERR_SPACE:
    return -ENOSPC;
  case SENSEI_SECURE_ERR_STATE:
    return -ENOTCONN;
  case SENSEI_SECURE_ERR_SENDER:
    return -ENOMEM; /* sender table full */
  case SENSEI_SECURE_ERR_TXKEY:
    return -EPROTO; /* the node has no key for the counter (the chain failed): not a refusal of a peer */
  default:
    return -EIO;
  }
}

#define CTR_MAX 0xFFFFFFFFu
#define NONE_EPOCH 0xFFFFFFFFu
#define MAX_EPOCH_IDX (1u << 26) /* an epoch of a 32-bit counter is below this */

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;

  while (n--) {
    *v++ = 0;
  }
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t now_up(const struct sensei_e2e *e) { return e->ops->uptime_s ? e->ops->uptime_s(e->user) : 0; }

static uint32_t now_unix(const struct sensei_e2e *e) { return e->ops->unix_s ? e->ops->unix_s(e->user) : 0; }

static bool ratchet_ok(const struct sensei_e2e *e) { return sensei_ratchet_available(e->c); }

/* ---- the ratchet record e2e_rat (version 1, 156 B, integers little endian) ----
 *   0  u8 version (1)        1  u8 flags: bit 0 prev, bit 1 pending      2  u16 age of the generation, hours
 *   4  u32 key_id            8  u32 gen          12 u32 gen_start_unix
 *  16  root[32]             48  k_down[16]
 *  64  u32 up_base          68  u32 up_idx       72  up_ck[32]
 * 104  prev_k_down[16]     120  u32 pend_up_base 124 pend_priv[32]
 * Generation 0 has a record only while a request is pending (root = R_0, the rest 0). */

#define RAT_VERSION 1u
#define RAT_PREV 1u
#define RAT_PENDING 2u
#define AGE_MAX 0xFFFFu /* 7.4 years */

/* Age of the generation in hours, rounded up (a mix may come a little early, never late), at most AGE_MAX. The uptime
 * counts from the load: the age that was stored then is part of gen_start_up (rat_decode). */
static uint16_t gen_age_h(const struct sensei_e2e *e, const struct sensei_e2e_rat *r) {
  uint32_t s = now_up(e) - r->gen_start_up; /* modular: gen_start_up may lie "before" the boot */
  uint32_t h = s / 3600u + (s % 3600u != 0u ? 1u : 0u);

  return (uint16_t)(h > AGE_MAX ? AGE_MAX : h);
}

static void rat_encode(const struct sensei_e2e *e, const struct sensei_e2e_rat *r, uint8_t rec[SENSEI_E2E_RAT_SIZE]) {
  uint16_t age = gen_age_h(e, r);

  memset(rec, 0, SENSEI_E2E_RAT_SIZE);
  rec[0] = RAT_VERSION;
  rec[1] = (uint8_t)((r->prev ? RAT_PREV : 0u) | (r->pending ? RAT_PENDING : 0u));
  rec[2] = (uint8_t)age;
  rec[3] = (uint8_t)(age >> 8);
  put32(rec + 4, e->s.key_id);
  put32(rec + 8, r->gen);
  put32(rec + 12, r->gen_start_unix);
  memcpy(rec + 16, r->root, 32);
  if (r->gen != 0) {
    memcpy(rec + 48, r->k_down, 16);
    put32(rec + 64, r->up.up_base);
    put32(rec + 68, r->up.idx);
    memcpy(rec + 72, r->up.ck, 32);
    if (r->prev) {
      memcpy(rec + 104, r->prev_k_down, 16);
    }
  }
  if (r->pending) {
    put32(rec + 120, r->pend_up_base);
    memcpy(rec + 124, r->pend_priv, 32);
  }
}

/* 0, 1 (a record of another key: old, ignored) or -EBADMSG (not a valid record). The state goes to r. */
static int rat_decode(const struct sensei_e2e *e, const uint8_t rec[SENSEI_E2E_RAT_SIZE], struct sensei_e2e_rat *r) {
  uint32_t gen = get32(rec + 8), up_base = get32(rec + 64), up_idx = get32(rec + 68), pend = get32(rec + 120);
  uint8_t flags = rec[1];
  uint32_t age_h = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8);

  if (rec[0] != RAT_VERSION || (flags & ~(RAT_PREV | RAT_PENDING)) != 0) {
    return -EBADMSG;
  }
  if (get32(rec + 4) != e->s.key_id) {
    return 1;
  }
  if (gen == 0 ? ((flags & RAT_PENDING) == 0 || (flags & RAT_PREV) != 0)
               : (up_base == 0 || up_base >= MAX_EPOCH_IDX || up_idx >= MAX_EPOCH_IDX - up_base)) {
    return -EBADMSG;
  }
  if ((flags & RAT_PENDING) && (pend == 0 || pend >= MAX_EPOCH_IDX || (gen != 0 && pend <= up_base))) {
    return -EBADMSG;
  }
  memset(r, 0, sizeof(*r));
  r->gen = gen;
  r->gen_start_unix = get32(rec + 12);
  r->gen_start_up = now_up(e) - age_h * 3600u; /* the generation started this long before now (a write is the clock) */
  memcpy(r->root, rec + 16, 32);
  r->prev = (flags & RAT_PREV) != 0;
  r->pending = (flags & RAT_PENDING) != 0;
  if (gen != 0) {
    memcpy(r->k_down, rec + 48, 16);
    sensei_ratchet_chain_init(&r->up, rec + 72, up_base);
    r->up.idx = up_idx;
    memcpy(r->prev_k_down, rec + 104, 16);
  }
  if (r->pending) {
    r->pend_up_base = pend;
    memcpy(r->pend_priv, rec + 124, 32);
  }
  return 0;
}

/* Store the record of state r (the state in RAM follows once this returned 0). Every write carries the age of the
 * generation, so a node that restarts often still counts the time of the generation (up to the time since the last
 * write). */
static int rat_save(struct sensei_e2e *e, const struct sensei_e2e_rat *r) {
  int err = 0;

  if (e->ops->save_rat) {
    uint8_t rec[SENSEI_E2E_RAT_SIZE];

    rat_encode(e, r, rec);
    err = e->ops->save_rat(e->user, e->s.key_id, rec, sizeof(rec));
    wipe(rec, sizeof(rec));
  }
  if (err == 0) {
    e->rat_stored = true;
  }
  return err;
}

static int rat_delete(struct sensei_e2e *e) {
  int err = 0;

  if (e->rat_stored && e->ops->save_rat) {
    err = e->ops->save_rat(e->user, e->s.key_id, NULL, 0);
  }
  if (err == 0) {
    e->rat_stored = false;
  }
  return err;
}

/* ---- the keyring of the session ---- */

static int ring_tx_key(void *user, uint32_t counter, uint32_t *gen, uint8_t key[SENSEI_SECURE_KEY_SIZE]) {
  struct sensei_e2e *e = user;

  *gen = e->r.gen;
  /* The counter and the chain key are in flash before a key is given out: the counter is at or below the stored mark,
   * and the stored chain index is at or below the epoch in use. Not reached; a node that gets here has a bug, and it
   * does not send. */
  if (counter > e->nv.tx_reserved) {
    return -EINVAL;
  }
  if (e->r.gen == 0) {
    return 0;
  }
  if (!e->flash_idx_valid || sensei_ratchet_epoch(counter) < e->r.up.up_base ||
      e->flash_idx > sensei_ratchet_epoch(counter) - e->r.up.up_base) {
    return -EINVAL;
  }
  return sensei_ratchet_chain_key(e->c, &e->r.up, counter, true, SENSEI_RATCHET_MAX_SKIP, key);
}

/* cppcheck-suppress constParameterCallback */
static int ring_rx_key(void *user, bool ratchet, unsigned gen_bits, uint32_t counter,
                       uint8_t key[SENSEI_SECURE_KEY_SIZE]) {
  const struct sensei_e2e *e = user;
  const struct sensei_e2e_rat *r = &e->r;

  (void)counter;
  if (!ratchet) { /* generation 0 format: the static key, while the previous generation is generation 0 */
    return r->gen == 0 || (r->gen == 1 && r->prev) ? 1 : SENSEI_SECURE_ERR_GEN;
  }
  if (r->gen == 0) {
    return SENSEI_SECURE_ERR_GEN;
  }
  if (gen_bits == (r->gen & 3u)) {
    memcpy(key, r->k_down, SENSEI_SECURE_KEY_SIZE);
    return 0;
  }
  if (r->gen >= 2 && r->prev && gen_bits == ((r->gen - 1) & 3u)) {
    memcpy(key, r->prev_k_down, SENSEI_SECURE_KEY_SIZE);
    return 0;
  }
  return SENSEI_SECURE_ERR_GEN;
}

/* The counters are persisted in blocks of whole epochs. The block of the counter first ends at the end of its epoch
 * (res = first | 63). While the persists come less than a minute apart (streaming) the block doubles every time, up to
 * SENSEI_E2E_BLOCK_EPOCHS_MAX epochs, so that the flash is written about once a minute at most, whatever the rate; a
 * persist after a minute or more halves it (a slow sender comes down to one epoch, a fast one stays near the size that
 * lasts a minute); a restart starts with one epoch. The price: the chain key in flash covers the
 * epochs of the block that were sent already, up to SENSEI_E2E_BLOCK_EPOCHS_MAX * 64 messages are open to someone who
 * reads the flash. From generation 1 on the chain key of the first epoch of the block goes to flash before the counter
 * mark: a crash in between leaves the old mark (nothing of the block was used) and a key that is not behind it. */
static int ring_tx_block(void *user, uint32_t first, uint32_t *reserved) {
  struct sensei_e2e *e = user;
  uint32_t t = now_up(e);
  uint64_t res = (uint64_t)(first | 63u);
  uint32_t epochs = 1u;
  int err;

  if (e->persist_seen) {
    if ((uint32_t)(t - e->persist_t) < SENSEI_E2E_PERSIST_FAST_S) {
      epochs = e->block_epochs * 2u > SENSEI_E2E_BLOCK_EPOCHS_MAX ? SENSEI_E2E_BLOCK_EPOCHS_MAX : e->block_epochs * 2u;
    } else {
      epochs = e->block_epochs > 1u ? e->block_epochs / 2u : 1u;
    }
  }
  res += (uint64_t)(epochs - 1u) * SENSEI_RATCHET_EPOCH_SIZE;
  if (res > CTR_MAX) {
    res = CTR_MAX;
  }
  if (e->r.gen != 0) {
    uint32_t ep = sensei_ratchet_epoch(first);

    if (ep < e->r.up.up_base) {
      return -EINVAL; /* sensei_e2e_seal() moves the counter up first: not reached */
    }
    err = sensei_ratchet_chain_advance(e->c, &e->r.up, ep - e->r.up.up_base, SENSEI_RATCHET_MAX_SKIP);
    if (err) {
      return -EIO;
    }
    if (!e->flash_idx_valid || e->flash_idx != e->r.up.idx) {
      err = rat_save(e, &e->r);
      if (err) {
        return err;
      }
      e->flash_idx = e->r.up.idx;
      e->flash_idx_valid = true;
    }
  }
  err = e->ops->save_tx ? e->ops->save_tx(e->user, (uint32_t)res) : 0;
  if (err) {
    return err;
  }
  e->nv.tx_reserved = (uint32_t)res;
  e->block_epochs = epochs;
  e->persist_seen = true;
  e->persist_t = t;
  *reserved = (uint32_t)res;
  return 0;
}

static int persist_rx(void *user, uint32_t sender, uint32_t highest) {
  struct sensei_e2e *e = user;
  struct sensei_secure_rx_state tab[SENSEI_SECURE_MAX_SENDERS];
  size_t n = e->nv.n_rx;
  size_t i;
  int err = 0;

  memcpy(tab, e->nv.rx, sizeof(tab));
  for (i = 0; i < n && tab[i].sender != sender; i++) {
  }
  if (i == n) {
    if (n == SENSEI_SECURE_MAX_SENDERS) {
      return -ENOMEM;
    }
    n++;
  }
  tab[i].sender = sender;
  tab[i].highest = highest;
  if (e->ops->save_rx) {
    err = e->ops->save_rx(e->user, tab, n);
  }
  if (err == 0) { /* the table in RAM follows the table in flash */
    memcpy(e->nv.rx, tab, sizeof(tab));
    e->nv.n_rx = n;
  }
  return err;
}

static int make_pair(struct sensei_e2e *e) {
  int err = sensei_secure_keygen(e->c, e->nv.priv, e->node_pub);

  if (err) {
    return -EIO;
  }
  err = e->ops->save_priv ? e->ops->save_priv(e->user, e->nv.priv) : 0;
  if (err) { /* a key that is not stored is not kept */
    memset(e->nv.priv, 0, sizeof(e->nv.priv));
    memset(e->node_pub, 0, sizeof(e->node_pub));
    return err;
  }
  e->nv.have_priv = true;
  return 0;
}

static int read_pub(struct sensei_e2e *e) {
  sensei_secure_key_t k = 0;
  int err = e->c->ecdh_import(e->c->ctx, e->nv.priv, sizeof(e->nv.priv), &k);

  if (err) {
    return -EIO;
  }
  err = e->c->ecdh_public(e->c->ctx, k, e->node_pub);
  (void)e->c->key_destroy(e->c->ctx, k);
  return err ? -EIO : 0;
}

/* The static keys of a collector key (generation 0). -EINVAL for a key that cannot be used. */
static int derive_keys(struct sensei_e2e *e, const uint8_t *backend_pub, struct sensei_secure_keys *keys) {
  sensei_secure_key_t own = 0;
  int err;

  if (e->c->ecdh_import(e->c->ctx, e->nv.priv, sizeof(e->nv.priv), &own) != 0) {
    return -EIO;
  }
  err = sensei_secure_derive(e->c, own, SENSEI_SECURE_ROLE_NODE, e->node_pub, backend_pub, keys);
  (void)e->c->key_destroy(e->c->ctx, own);
  return err ? -EINVAL : 0;
}

int sensei_e2e_key_id_of(const struct sensei_secure_crypto *c, const uint8_t priv[SENSEI_SECURE_PRIV_SIZE],
                         const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE], uint32_t *key_id) {
  sensei_secure_key_t own = 0;
  struct sensei_secure_keys keys;
  uint8_t pub[SENSEI_SECURE_PUB_SIZE];
  int err = -EIO;

  if (c->ecdh_import(c->ctx, priv, SENSEI_SECURE_PRIV_SIZE, &own) != 0) {
    return -EIO;
  }
  if (c->ecdh_public(c->ctx, own, pub) == 0) {
    err = sensei_secure_derive(c, own, SENSEI_SECURE_ROLE_NODE, pub, backend_pub, &keys) == 0 ? 0 : -EINVAL;
    if (err == 0) {
      *key_id = keys.key_id;
    }
    wipe(&keys, sizeof(keys));
  }
  (void)c->key_destroy(c->ctx, own);
  return err;
}

/* Open the session on the static keys. The counters (nv) do not depend on the key: a new key goes on above the sent
 * high-water mark, and the senders keep their windows. r.gen 0: R_0 of these keys is taken. */
static int open_session(struct sensei_e2e *e, const struct sensei_secure_keys *keys) {
  struct sensei_secure_store store = {.rx = persist_rx, .user = e, .tx_block = ring_tx_block};
  struct sensei_secure_keyring ring = {.user = e, .tx_key = ring_tx_key, .rx_key = ring_rx_key};

  if (e->ready) {
    sensei_secure_close(&e->s);
    e->ready = false;
  }
  if (sensei_secure_init(&e->s, e->c, SENSEI_SECURE_ROLE_NODE, keys, 0, e->nv.tx_reserved, e->ctr_step, e->nv.rx,
                         e->nv.n_rx, &store) != 0) {
    return -EIO;
  }
  if (ratchet_ok(e)) {
    sensei_secure_set_keyring(&e->s, &ring);
    if (e->r.gen == 0 && sensei_ratchet_root0(e->c, keys, e->r.root) != 0) {
      sensei_secure_close(&e->s);
      return -EIO;
    }
  }
  e->ready = true;
  return 0;
}

static int start(struct sensei_e2e *e, const uint8_t *backend_pub) {
  struct sensei_secure_keys keys;
  int err = derive_keys(e, backend_pub, &keys);

  if (err == 0) {
    err = open_session(e, &keys);
  }
  wipe(&keys, sizeof(keys));
  return err;
}

/* The ratchet state from flash, once the session is open (key id known). */
static void rat_load(struct sensei_e2e *e) {
  struct sensei_e2e_rat r;
  int rc;

  if (e->nv.rat_unreadable) {
    e->damaged = true;
    e->rat_stored = true;
    return;
  }
  if (!e->nv.have_rat) {
    return;
  }
  e->rat_stored = true;
  rc = rat_decode(e, e->nv.rat, &r);
  if (rc == 1) { /* a record of an old key: ignored, the next save replaces it */
    return;
  }
  if (rc == 0 && r.gen == 0 && !ratchet_ok(e)) {
    /* A generation 0 record only holds a request that is pending: a build without the ratchet never asks, and no key
     * depends on it. It is dropped (the next save or provisioning replaces it); only a state of generation 1 or later
     * must not be run as generation 0. */
    wipe(&r, sizeof(r));
    return;
  }
  if (rc != 0 || !ratchet_ok(e)) { /* a record that is not valid, or one this build cannot use: no fallback */
    e->damaged = true;
    wipe(&r, sizeof(r));
    return;
  }
  if (r.pending && sensei_ratchet_public(e->c, r.pend_priv, r.pend_pub) != 0) {
    e->damaged = true;
    wipe(&r, sizeof(r));
    return;
  }
  e->r = r;
  e->flash_idx = r.up.idx;
  e->flash_idx_valid = r.gen != 0;
  wipe(&r, sizeof(r));
}

static int init_body(struct sensei_e2e *e, const struct sensei_secure_crypto *c, const struct sensei_e2e_ops *ops,
                     void *user, const struct sensei_e2e_nv *nv, uint32_t ctr_step) {
  int err;

  memset(e, 0, sizeof(*e));
  e->c = c;
  e->ops = ops;
  e->user = user;
  e->ctr_step = ctr_step ? ctr_step : 64;
  e->nv = *nv;
  e->epoch_cur = NONE_EPOCH;
  e->cfg = (struct sensei_e2e_cfg){.rekey = true,
                                   .rekey_days = SENSEI_E2E_REKEY_DAYS,
                                   .retry_h = SENSEI_E2E_REKEY_RETRY_H,
                                   .epoch_max_s = SENSEI_E2E_EPOCH_MAX_S};
  e->r.gen_start_up = now_up(e);
  e->rat_stored = nv->have_rat || nv->rat_unreadable;
  if (nv->key_unreadable) {
    memset(e->nv.priv, 0, sizeof(e->nv.priv));
    e->nv.have_priv = false;
    return -EACCES;
  }
  if (!e->nv.have_priv) {
    err = make_pair(e);
  } else {
    err = read_pub(e);
    if (err && e->nv.priv_sealed) { /* a sealed key is not damaged: a crypto error, keep the record, e2e stays off */
      memset(e->nv.priv, 0, sizeof(e->nv.priv));
      e->nv.have_priv = false;
      return -EIO;
    }
    if (err) { /* a damaged plain key: start over, the collector has to be provisioned again */
      e->nv.have_backend = false;
      err = make_pair(e);
    }
  }
  if (err) {
    return err;
  }
  if (e->nv.have_backend) {
    err = start(e, e->nv.backend_pub);
    if (err == 0) {
      rat_load(e);
    }
  }
  return err;
}

int sensei_e2e_init(struct sensei_e2e *e, const struct sensei_secure_crypto *c, const struct sensei_e2e_ops *ops,
                    void *user, struct sensei_e2e_nv *nv, uint32_t ctr_step) {
  int err = init_body(e, c, ops, user, nv, ctr_step);

  /* The secrets are in e now (e->r, e->nv.priv): the caller's copy goes, so that it does not keep R_g, the chain key,
   * the downlink key, the pending key or the private key in RAM. The flags stay. */
  wipe(e->nv.rat, sizeof(e->nv.rat));
  e->nv.have_rat = false;
  wipe(nv->rat, sizeof(nv->rat));
  nv->have_rat = false;
  wipe(nv->priv, sizeof(nv->priv));
  return err;
}

/* Back to generation 0 in RAM (the state of a node that was just provisioned). */
static void rat_reset_ram(struct sensei_e2e *e) {
  wipe(&e->r, sizeof(e->r));
  e->damaged = false;
  e->flash_idx_valid = false;
  e->sent_valid = false;
  e->epoch_cur = NONE_EPOCH;
  e->r.gen_start_up = now_up(e);
}

int sensei_e2e_provision(struct sensei_e2e *e, const uint8_t *backend_pub, size_t len) {
  struct sensei_secure_keys keys;
  int err;

  if (len != SENSEI_SECURE_PUB_SIZE || backend_pub[0] != 4) {
    return -EINVAL;
  }
  if (!e->nv.have_priv) {
    return -ENOENT;
  }
  if (e->dirty) { /* a reset stopped half way: the key pair in RAM may be gone from flash */
    return -EBUSY;
  }
  err = derive_keys(e, backend_pub, &keys);
  if (err) {
    return err; /* the old session stays open */
  }
  /* The collector key first, then the ratchet record: a record that is left behind belongs to the old key id and is
   * dropped at the next start (and if the key id is the same, the delete is retried by calling this again). */
  err = e->ops->save_backend ? e->ops->save_backend(e->user, backend_pub) : 0;
  if (err) {
    wipe(&keys, sizeof(keys));
    return err; /* nothing changed */
  }
  memcpy(e->nv.backend_pub, backend_pub, SENSEI_SECURE_PUB_SIZE);
  e->nv.have_backend = true;
  err = rat_delete(e);
  if (err) { /* flash has the new key and maybe the old state: do not talk until this is repeated */
    e->damaged = true;
    wipe(&keys, sizeof(keys));
    return err;
  }
  rat_reset_ram(e);
  err = open_session(e, &keys);
  wipe(&keys, sizeof(keys));
  return err;
}

int sensei_e2e_reset(struct sensei_e2e *e) {
  struct sensei_e2e_nv empty;

  if (e->ops->erase) {
    int err = e->ops->erase(e->user);

    if (err) { /* refused: keep the session and the state */
      e->dirty = true;
      return err;
    }
  }
  e->dirty = false;
  memset(&empty, 0, sizeof(empty));
  if (e->ready) {
    sensei_secure_close(&e->s);
    e->ready = false;
  }
  e->nv = empty;
  e->rat_stored = false;
  rat_reset_ram(e);
  return make_pair(e);
}

size_t sensei_e2e_senders_used(const struct sensei_e2e *e) { return e->nv.n_rx; }

size_t sensei_e2e_senders_max(void) { return SENSEI_SECURE_MAX_SENDERS; }

int sensei_e2e_clear_senders(struct sensei_e2e *e) {
  uint32_t floor = e->nv.rx_floor;
  int err = 0;

  for (size_t i = 0; i < e->nv.n_rx; i++) {
    if (e->nv.rx[i].highest > floor) {
      floor = e->nv.rx[i].highest;
    }
  }
  /* The floor first: if the table write fails after it, the floor only refuses what the table
   * refuses anyway. */
  if (floor != e->nv.rx_floor) {
    err = e->ops->save_floor ? e->ops->save_floor(e->user, floor) : 0;
    if (err) {
      return err;
    }
    e->nv.rx_floor = floor;
  }
  if (e->ops->save_rx) {
    err = e->ops->save_rx(e->user, e->nv.rx, 0);
  }
  if (err) {
    return err;
  }
  memset(e->nv.rx, 0, sizeof(e->nv.rx));
  e->nv.n_rx = 0;
  return e->nv.have_backend ? start(e, e->nv.backend_pub) : 0; /* the open session still holds the old table */
}

bool sensei_e2e_ready(const struct sensei_e2e *e) { return e->ready; }

uint32_t sensei_e2e_key_id(const struct sensei_e2e *e) { return e->ready ? e->s.key_id : 0; }

/* From generation 1 on, move the counter to where the chain can serve it: not before the epoch of the chain (a restart,
 * a switch of generation), the first counter of a run on an epoch boundary (a restart starts a new epoch), and out of
 * an epoch that was in use longer than epoch_max_s. The hook of the keyring could not serve a counter before the chain
 * and the seal would fail for ever. */
static int place_counter(struct sensei_e2e *e) {
  uint64_t next = (uint64_t)e->s.tx_last + 1u;
  uint64_t want = next;

  if (e->r.gen == 0) {
    return 0;
  }
  if (e->epoch_cur == NONE_EPOCH) {
    want = (want + 63u) & ~(uint64_t)63u;
  } else if (e->cfg.epoch_max_s && sensei_ratchet_epoch((uint32_t)next) == e->epoch_cur &&
             (uint32_t)(now_up(e) - e->epoch_t0) >= e->cfg.epoch_max_s) {
    want = ((uint64_t)e->epoch_cur + 1u) << SENSEI_RATCHET_EPOCH_BITS;
  }
  if (want < ((uint64_t)e->r.up.up_base + e->r.up.idx) << SENSEI_RATCHET_EPOCH_BITS) {
    want = ((uint64_t)e->r.up.up_base + e->r.up.idx) << SENSEI_RATCHET_EPOCH_BITS;
  }
  if (want > CTR_MAX - 1u) {
    return -EOVERFLOW;
  }
  if (want > next) {
    (void)sensei_secure_tx_jump(&e->s, (uint32_t)want);
  }
  return 0;
}

int sensei_e2e_seal(struct sensei_e2e *e, enum sensei_secure_kind kind, const uint8_t *pt, size_t len, uint8_t *out,
                    size_t cap) {
  int n;

  if (!e->ready) {
    return -ENOTCONN;
  }
  if (e->damaged) {
    e->refused++;
    return -EPERM;
  }
  n = place_counter(e);
  if (n) {
    return n;
  }
  n = sensei_secure_seal(&e->s, kind, NULL, pt, len, out, cap);
  if (n >= 0 && e->r.gen != 0) {
    uint32_t ep = sensei_ratchet_epoch(e->s.tx_last);

    if (ep != e->epoch_cur) {
      e->epoch_cur = ep;
      e->epoch_t0 = now_up(e);
    }
  }
  return n < 0 ? sensei_e2e_errno(n) : n;
}

/* The counter of a backend to node envelope (the last 4 bytes of its header), or 0. */
static uint32_t peek_down_counter(const uint8_t *in, size_t len) {
  size_t n;

  if (!sensei_secure_is_sealed(in, len) || (in[0] & 1u) == 0) {
    return 0;
  }
  n = ((in[0] & 2u) ? SENSEI_SECURE_HEADER_NODE : SENSEI_SECURE_HEADER) + SENSEI_SECURE_SENDER_SIZE;
  if (len < n + SENSEI_SECURE_TAG_SIZE) {
    return 0;
  }
  return (uint32_t)in[n - 4] | ((uint32_t)in[n - 3] << 8) | ((uint32_t)in[n - 2] << 16) | ((uint32_t)in[n - 1] << 24);
}

int sensei_e2e_open(struct sensei_e2e *e, enum sensei_secure_kind want, const uint8_t *in, size_t len, uint8_t *out,
                    size_t cap, size_t *out_len) {
  uint32_t ctr;

  if (!e->ready) {
    return -ENOTCONN;
  }
  if (e->damaged) {
    e->refused++;
    return -EPERM;
  }
  ctr = peek_down_counter(in, len);
  if (ctr != 0 && ctr <= e->nv.rx_floor) {
    return -EALREADY; /* at or below what the cleared senders had: a replay */
  }
  return sensei_e2e_errno(sensei_secure_open(&e->s, want, NULL, in, len, out, cap, out_len, NULL));
}

/* ---- forward secrecy: requests, answers, switch ---- */

void sensei_e2e_configure(struct sensei_e2e *e, const struct sensei_e2e_cfg *cfg) { e->cfg = *cfg; }

uint32_t sensei_e2e_gen(const struct sensei_e2e *e) { return e->r.gen; }

void sensei_e2e_status(const struct sensei_e2e *e, struct sensei_e2e_status *st) {
  memset(st, 0, sizeof(*st));
  st->gen = e->r.gen;
  st->pending = e->r.pending;
  st->prev = e->r.prev;
  st->damaged = e->damaged;
  if (e->r.gen != 0) {
    st->up_base = e->r.up.up_base;
    st->up_idx = e->r.up.idx;
  }
  st->rekeys = e->rekeys;
  st->refused = e->refused;
}

/* The mix is due: generation 0 after provisioning, the period, a long chain. */
static bool rekey_trigger(struct sensei_e2e *e) {
  uint64_t period = (uint64_t)e->cfg.rekey_days * 86400u;
  uint32_t unix_now, ep;

  if (e->r.gen == 0) {
    return true;
  }
  ep = sensei_ratchet_epoch(e->s.tx_last);
  if (ep >= e->r.up.up_base && ep - e->r.up.up_base >= SENSEI_E2E_REKEY_EPOCHS) {
    return true;
  }
  if (period == 0) {
    return false;
  }
  unix_now = now_unix(e);
  if (unix_now != 0 && e->r.gen_start_unix == 0) {
    /* The clock was not set when the generation started: back-date the start by the age (the stored age and the
     * uptime of this boot). Stored with the next record. */
    uint32_t age_s = now_up(e) - e->r.gen_start_up;

    e->r.gen_start_unix = unix_now > age_s ? unix_now - age_s : 1u;
  }
  if (unix_now != 0 && e->r.gen_start_unix != 0 && unix_now >= e->r.gen_start_unix) {
    return (uint64_t)(unix_now - e->r.gen_start_unix) >= period;
  }
  /* no clock, or one that is behind the start of the generation (it was wrong then): count the uptime */
  return (uint64_t)(uint32_t)(now_up(e) - e->r.gen_start_up) >= period;
}

static bool can_mix(const struct sensei_e2e *e) { return e->ready && e->cfg.rekey && ratchet_ok(e) && !e->damaged; }

/* A new request: ephemeral key, up_base, the previous downlink key dropped, stored before anything is sent. */
static int rekey_create(struct sensei_e2e *e) {
  struct sensei_e2e_rat n = e->r;
  uint32_t used = e->s.tx_last > e->s.tx_reserved ? e->s.tx_last : e->s.tx_reserved;
  uint32_t up_base = sensei_ratchet_epoch(used) + 1u;
  int err;

  if (n.gen != 0 && up_base <= n.up.up_base) {
    up_base = n.up.up_base + 1u;
  }
  if (up_base >= MAX_EPOCH_IDX) {
    return -ERANGE;
  }
  if (e->c->x25519_keygen(e->c->ctx, n.pend_priv, n.pend_pub) != 0) {
    wipe(&n, sizeof(n));
    return -EIO;
  }
  n.pend_up_base = up_base;
  n.pending = true;
  n.prev = false; /* the downlink key of the previous generation lives until the next request */
  wipe(n.prev_k_down, sizeof(n.prev_k_down));
  err = rat_save(e, &n);
  if (err == 0) {
    wipe(&e->r, sizeof(e->r));
    e->r = n;
    e->sent_valid = false;
  }
  wipe(&n, sizeof(n));
  return err;
}

static size_t put_varint(uint8_t *p, uint32_t v) {
  size_t n = 0;

  while (v >= 0x80u) {
    p[n++] = (uint8_t)(v | 0x80u);
    v >>= 7;
  }
  p[n++] = (uint8_t)v;
  return n;
}

/* Uplink.rekey (field 14): tag 0x72, length, RekeyRequest{gen = 1, pub = 2, up_base = 3}. */
static size_t rekey_encode(const struct sensei_e2e *e, uint8_t out[SENSEI_E2E_REKEY_MAX]) {
  uint8_t in[SENSEI_E2E_REKEY_MAX];
  size_t n = 0;

  in[n++] = 0x08;
  n += put_varint(in + n, e->r.gen + 1u);
  in[n++] = 0x12;
  in[n++] = SENSEI_RATCHET_KEY_SIZE;
  memcpy(in + n, e->r.pend_pub, SENSEI_RATCHET_KEY_SIZE);
  n += SENSEI_RATCHET_KEY_SIZE;
  in[n++] = 0x18;
  n += put_varint(in + n, e->r.pend_up_base);
  out[0] = 0x72;
  out[1] = (uint8_t)n;
  memcpy(out + 2, in, n);
  return n + 2;
}

int sensei_e2e_rekey_due(struct sensei_e2e *e, uint8_t *out, size_t cap) {
  uint8_t enc[SENSEI_E2E_REKEY_MAX];
  size_t n;
  uint32_t t;

  if (!e->ready) {
    return -ENOTCONN;
  }
  if (e->damaged) {
    return -EPERM;
  }
  if (!e->cfg.rekey || !ratchet_ok(e)) {
    return 0;
  }
  t = now_up(e);
  if (e->r.pending) {
    if (e->sent_valid && (uint64_t)(uint32_t)(t - e->sent_t) < (uint64_t)e->cfg.retry_h * 3600u) {
      return 0;
    }
  } else {
    int err;

    if (!rekey_trigger(e)) {
      return 0;
    }
    err = rekey_create(e);
    if (err) {
      return err;
    }
  }
  n = rekey_encode(e, enc);
  if (cap < n) {
    return -ENOSPC;
  }
  memcpy(out, enc, n);
  e->sent_valid = true;
  e->sent_t = t;
  return (int)n;
}

void sensei_e2e_rekey_unsent(struct sensei_e2e *e) { e->sent_valid = false; }

int sensei_e2e_rekey_now(struct sensei_e2e *e) {
  if (!e->ready) {
    return -ENOTCONN;
  }
  if (e->damaged) {
    return -EPERM;
  }
  if (!can_mix(e)) {
    return -ENOTSUP;
  }
  if (e->r.pending) {
    e->sent_valid = false; /* send it again with the next Uplink */
    return -EALREADY;
  }
  return rekey_create(e);
}

int sensei_e2e_rekey_answer(struct sensei_e2e *e, uint32_t gen, const uint8_t *pub, size_t pub_len, uint32_t up_base) {
  struct sensei_e2e_rat n;
  struct sensei_ratchet_mix mix;
  uint64_t next;
  int err;

  if (!e->ready) {
    return -ENOTCONN;
  }
  if (e->damaged) {
    return -EPERM;
  }
  if (!e->r.pending || gen <= e->r.gen) { /* not waiting, or an answer that was handled already */
    return -EALREADY;
  }
  if (gen != e->r.gen + 1u || up_base != e->r.pend_up_base || pub_len != SENSEI_RATCHET_KEY_SIZE || pub == NULL) {
    return -EBADMSG;
  }
  err = sensei_ratchet_mix(e->c, SENSEI_SECURE_ROLE_NODE, e->r.pend_priv, e->r.root, e->s.key_id, gen, up_base,
                           e->r.pend_pub, pub, &mix);
  if (err) {
    return err == SENSEI_SECURE_ERR_DH ? -EACCES : -EIO;
  }
  /* The first counter of the new generation: above everything sent, and not before the chain starts. */
  next = (uint64_t)e->s.tx_last + 1u;
  if (next < ((uint64_t)up_base << SENSEI_RATCHET_EPOCH_BITS)) {
    next = (uint64_t)up_base << SENSEI_RATCHET_EPOCH_BITS;
  }
  if (next > CTR_MAX - 1u) {
    wipe(&mix, sizeof(mix));
    return -EOVERFLOW;
  }
  memset(&n, 0, sizeof(n));
  n.gen = gen;
  n.gen_start_unix = now_unix(e);
  n.gen_start_up = now_up(e);
  memcpy(n.root, mix.root, sizeof(n.root));
  memcpy(n.k_down, mix.k_down, sizeof(n.k_down));
  sensei_ratchet_chain_init(&n.up, mix.ck, up_base);
  err = sensei_ratchet_chain_advance(e->c, &n.up, sensei_ratchet_epoch((uint32_t)next) - up_base,
                                     SENSEI_RATCHET_MAX_SKIP);
  if (err == 0) {
    n.prev = true; /* generation 1: the previous one is the static key (the legacy format) */
    if (e->r.gen != 0) {
      memcpy(n.prev_k_down, e->r.k_down, sizeof(n.prev_k_down));
    }
    err = rat_save(e, &n); /* one atomic write: the old state, or the new one */
  } else {
    err = -EIO;
  }
  wipe(&mix, sizeof(mix));
  if (err) {
    wipe(&n, sizeof(n));
    return err; /* still in generation g, still pending */
  }
  wipe(&e->r, sizeof(e->r)); /* the ephemeral key, R_g and the chain of generation g are gone */
  e->r = n;
  wipe(&n, sizeof(n));
  e->flash_idx = e->r.up.idx;
  e->flash_idx_valid = true;
  e->epoch_cur = NONE_EPOCH;
  e->sent_valid = false;
  e->rekeys++;
  (void)sensei_secure_tx_jump(&e->s, (uint32_t)next);
  return 0;
}
