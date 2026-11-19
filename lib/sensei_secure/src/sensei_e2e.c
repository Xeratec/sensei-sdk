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
  default:
    return -EIO;
  }
}

static int persist_tx(void *user, uint32_t reserved) {
  struct sensei_e2e *e = user;

  e->nv.tx_reserved = reserved;
  return e->ops->save_tx ? e->ops->save_tx(e->user, reserved) : 0;
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

/* Derive the keys for a collector key and open the session. The counters (nv) do not depend on the
 * key: a new key goes on above the sent high-water mark, and the senders keep their windows. */
static int start(struct sensei_e2e *e, const uint8_t *backend_pub) {
  struct sensei_secure_keys keys;
  sensei_secure_key_t own = 0;
  struct sensei_secure_store store = {.tx = persist_tx, .rx = persist_rx, .user = e};
  int err;

  if (e->c->ecdh_import(e->c->ctx, e->nv.priv, sizeof(e->nv.priv), &own) != 0) {
    return -EIO;
  }
  err = sensei_secure_derive(e->c, own, SENSEI_SECURE_ROLE_NODE, e->node_pub, backend_pub, &keys);
  (void)e->c->key_destroy(e->c->ctx, own);
  if (err) {
    return -EINVAL;
  }
  if (e->ready) {
    sensei_secure_close(&e->s);
    e->ready = false;
  }
  err = sensei_secure_init(&e->s, e->c, SENSEI_SECURE_ROLE_NODE, &keys, 0, e->nv.tx_reserved, e->ctr_step, e->nv.rx,
                           e->nv.n_rx, &store);
  memset(&keys, 0, sizeof(keys));
  if (err != 0) {
    return -EIO;
  }
  e->ready = true;
  return 0;
}

int sensei_e2e_init(struct sensei_e2e *e, const struct sensei_secure_crypto *c, const struct sensei_e2e_ops *ops,
                    void *user, const struct sensei_e2e_nv *nv, uint32_t ctr_step) {
  int err;

  memset(e, 0, sizeof(*e));
  e->c = c;
  e->ops = ops;
  e->user = user;
  e->ctr_step = ctr_step ? ctr_step : 64;
  e->nv = *nv;
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
  }
  return err;
}

int sensei_e2e_provision(struct sensei_e2e *e, const uint8_t *backend_pub, size_t len) {
  int err;

  if (len != SENSEI_SECURE_PUB_SIZE || backend_pub[0] != 4) {
    return -EINVAL;
  }
  if (!e->nv.have_priv) {
    return -ENOENT;
  }
  err = start(e, backend_pub);
  if (err) {
    return err; /* the old session stays open unless the new one failed to start */
  }
  memcpy(e->nv.backend_pub, backend_pub, SENSEI_SECURE_PUB_SIZE);
  e->nv.have_backend = true;
  return e->ops->save_backend ? e->ops->save_backend(e->user, backend_pub) : 0;
}

int sensei_e2e_reset(struct sensei_e2e *e) {
  struct sensei_e2e_nv empty;

  memset(&empty, 0, sizeof(empty));
  if (e->ready) {
    sensei_secure_close(&e->s);
    e->ready = false;
  }
  if (e->ops->erase) {
    e->ops->erase(e->user);
  }
  e->nv = empty;
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

int sensei_e2e_seal(struct sensei_e2e *e, enum sensei_secure_kind kind, const uint8_t *pt, size_t len, uint8_t *out,
                    size_t cap) {
  int n;

  if (!e->ready) {
    return -ENOTCONN;
  }
  n = sensei_secure_seal(&e->s, kind, NULL, pt, len, out, cap);
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
  ctr = peek_down_counter(in, len);
  if (ctr != 0 && ctr <= e->nv.rx_floor) {
    return -EALREADY; /* at or below what the cleared senders had: a replay */
  }
  return sensei_e2e_errno(sensei_secure_open(&e->s, want, NULL, in, len, out, cap, out_len, NULL));
}
