/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* SPAKE2+ commissioning: verifier (node) and prover (commissioner) sessions, the kind 5 channel, the verifier record.
 * C99, no Zephyr, no allocation. Mirror of sensei_sdk/commission.py: same bytes, vectors V9 to V15. */

#include "sensei_commission.h"

#include <string.h>

/* M and N of RFC 9383 section 4 (P-256) as uncompressed points. */
const uint8_t sensei_commission_m[SENSEI_COMMISSION_POINT_SIZE] = {
    0x04, 0x88, 0x6E, 0x2F, 0x97, 0xAC, 0xE4, 0x6E, 0x55, 0xBA, 0x9D, 0xD7, 0x24, 0x25, 0x79, 0xF2, 0x99,
    0x3B, 0x64, 0xE1, 0x6E, 0xF3, 0xDC, 0xAB, 0x95, 0xAF, 0xD4, 0x97, 0x33, 0x3D, 0x8F, 0xA1, 0x2F, 0x5F,
    0xF3, 0x55, 0x16, 0x3E, 0x43, 0xCE, 0x22, 0x4E, 0x0B, 0x0E, 0x65, 0xFF, 0x02, 0xAC, 0x8E, 0x5C, 0x7B,
    0xE0, 0x94, 0x19, 0xC7, 0x85, 0xE0, 0xCA, 0x54, 0x7D, 0x55, 0xA1, 0x2E, 0x2D, 0x20};
const uint8_t sensei_commission_n[SENSEI_COMMISSION_POINT_SIZE] = {
    0x04, 0xD8, 0xBB, 0xD6, 0xC6, 0x39, 0xC6, 0x29, 0x37, 0xB0, 0x4D, 0x99, 0x7F, 0x38, 0xC3, 0x77, 0x07,
    0x19, 0xC6, 0x29, 0xD7, 0x01, 0x4D, 0x49, 0xA2, 0x4B, 0x4F, 0x98, 0xBA, 0xA1, 0x29, 0x2B, 0x49, 0x07,
    0xD6, 0x0A, 0xA6, 0xBF, 0xAD, 0xE4, 0x50, 0x08, 0xA6, 0x36, 0x33, 0x7F, 0x51, 0x68, 0xC6, 0x4D, 0x9B,
    0xD3, 0x60, 0x34, 0x80, 0x8C, 0xD5, 0x64, 0x49, 0x0B, 0x1E, 0x65, 0x6E, 0xDB, 0xE7};

static const uint8_t ID_PROVER[SENSEI_COMMISSION_ID_SIZE] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'c', 'o', 'm',
                                                             'm', 'i', 's', 's', 'i', 'o', 'n', 'e', 'r'};
static const uint8_t ID_VERIFIER_PREFIX[11] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'n', 'o', 'd', 'e'};
static const uint8_t CONTEXT_LABEL[20] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'C', 'O', 'M',
                                          'M', 'I', 'S', 'S', 'I', 'O', 'N', '-', 'v', '1'};
static const uint8_t CHANNEL_INFO[7] = {'c', 'h', 'a', 'n', 'n', 'e', 'l'};
static const uint8_t POP_LABEL[24] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'C', 'O', 'M', 'M', 'I',
                                      'S', 'S', 'I', 'O', 'N', '-', 'p', 'o', 'p', '-', 'v', '2'};
static const uint8_t FP_NODE_LABEL[17] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'N', 'O',
                                          'D', 'E', '-', 'F', 'P', '-', 'v', '1'};
static const uint8_t FP_COLLECTOR_LABEL[22] = {'S', 'E', 'N', 'S', 'E', 'I', '-', 'C', 'O', 'L', 'L',
                                               'E', 'C', 'T', 'O', 'R', '-', 'F', 'P', '-', 'v', '1'};

#define ERR_FORMAT SENSEI_SECURE_ERR_FORMAT
#define ERR_AUTH SENSEI_SECURE_ERR_AUTH
#define ERR_CRYPTO SENSEI_SECURE_ERR_CRYPTO
#define ERR_STATE SENSEI_SECURE_ERR_STATE
#define ERR_STORE SENSEI_SECURE_ERR_STORE

/* A plain memset of a buffer that is not read again may be removed by the compiler. */
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;

  while (n--) {
    *v++ = 0;
  }
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;

  while (n--) {
    d |= (uint8_t)(*a++ ^ *b++);
  }
  return d == 0;
}

static bool is_zero(const uint8_t *a, size_t n) {
  uint8_t d = 0;

  while (n--) {
    d |= *a++;
  }
  return d == 0;
}

static void put32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64le(uint8_t *p, uint64_t v) {
  put32le(p, (uint32_t)v);
  put32le(p + 4, (uint32_t)(v >> 32));
}

static void put64be(uint8_t *p, uint64_t v) {
  for (int i = 7; i >= 0; i--) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

static bool backend_ok(const struct sensei_secure_crypto *c) {
  return c && c->random && c->sha256 && c->scalar_reduce && c->spake2p_check && c->spake2p_share && c->spake2p_zv &&
         c->hkdf && c->hmac_sha256 && c->aead_import && c->key_destroy && c->aead_encrypt && c->aead_decrypt;
}

/* ---- parameters and record ---- */

int sensei_commission_params_check(const struct sensei_commission_params *p) {
  if (p->suite != SENSEI_COMMISSION_SUITE || p->kdf != SENSEI_COMMISSION_KDF_SCRYPT ||
      p->log2n < SENSEI_COMMISSION_LOG2N_MIN || p->log2n > SENSEI_COMMISSION_LOG2N_MAX ||
      p->r != SENSEI_COMMISSION_SCRYPT_R || p->p != SENSEI_COMMISSION_SCRYPT_P) {
    return ERR_FORMAT;
  }
  return 0;
}

/* 0 if the record is acceptable (all that decode() checks). */
static int record_check(const struct sensei_secure_crypto *c, const struct sensei_commission_record *rec) {
  if (!c || !c->spake2p_check || sensei_commission_params_check(&rec->params) != 0 ||
      !sensei_secure_priv_valid(rec->w0) || rec->l[0] != 0x04 || c->spake2p_check(c->ctx, rec->l) != 0) {
    return ERR_FORMAT;
  }
  return 0;
}

int sensei_commission_record_decode(const struct sensei_secure_crypto *c, const uint8_t *data, size_t len,
                                    struct sensei_commission_record *rec) {
  struct sensei_commission_record r;

  if (!data || len != SENSEI_COMMISSION_RECORD_SIZE || data[0] != SENSEI_COMMISSION_RECORD_VERSION || data[6] != 0 ||
      data[7] != 0) {
    return ERR_FORMAT;
  }
  r.params.suite = data[1];
  r.params.kdf = data[2];
  r.params.log2n = data[3];
  r.params.r = data[4];
  r.params.p = data[5];
  memcpy(r.params.salt, data + 8, SENSEI_COMMISSION_SALT_SIZE);
  memcpy(r.w0, data + 24, SENSEI_COMMISSION_SCALAR_SIZE);
  memcpy(r.l, data + 56, SENSEI_COMMISSION_POINT_SIZE);
  if (record_check(c, &r) != 0) {
    wipe(&r, sizeof(r));
    return ERR_FORMAT;
  }
  *rec = r;
  wipe(&r, sizeof(r));
  return 0;
}

int sensei_commission_record_encode(const struct sensei_secure_crypto *c, const struct sensei_commission_record *rec,
                                    uint8_t out[SENSEI_COMMISSION_RECORD_SIZE]) {
  if (record_check(c, rec) != 0) {
    return ERR_FORMAT;
  }
  out[0] = SENSEI_COMMISSION_RECORD_VERSION;
  out[1] = rec->params.suite;
  out[2] = rec->params.kdf;
  out[3] = rec->params.log2n;
  out[4] = rec->params.r;
  out[5] = rec->params.p;
  out[6] = out[7] = 0;
  memcpy(out + 8, rec->params.salt, SENSEI_COMMISSION_SALT_SIZE);
  memcpy(out + 24, rec->w0, SENSEI_COMMISSION_SCALAR_SIZE);
  memcpy(out + 56, rec->l, SENSEI_COMMISSION_POINT_SIZE);
  return 0;
}

/* ---- pieces: context, identities, transcript, schedule, channel keys ---- */

void sensei_commission_context(const struct sensei_commission_params *p,
                               const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                               const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE],
                               uint8_t out[SENSEI_COMMISSION_CONTEXT_SIZE]) {
  memcpy(out, CONTEXT_LABEL, sizeof(CONTEXT_LABEL));
  out[20] = p->suite;
  out[21] = p->kdf;
  out[22] = p->log2n;
  out[23] = p->r;
  out[24] = p->p;
  memcpy(out + 25, p->salt, SENSEI_COMMISSION_SALT_SIZE);
  memcpy(out + 41, nonce_g, SENSEI_COMMISSION_NONCE_SIZE);
  memcpy(out + 57, nonce_n, SENSEI_COMMISSION_NONCE_SIZE);
}

void sensei_commission_id_verifier(uint64_t node_id, uint8_t out[SENSEI_COMMISSION_ID_SIZE]) {
  memcpy(out, ID_VERIFIER_PREFIX, sizeof(ID_VERIFIER_PREFIX));
  put64be(out + sizeof(ID_VERIFIER_PREFIX), node_id);
}

/* One item of the transcript: 8 B little endian length, then the bytes. */
static size_t tt_item(uint8_t *out, size_t pos, const uint8_t *data, size_t len) {
  put64le(out + pos, len);
  memcpy(out + pos + 8, data, len);
  return pos + 8 + len;
}

size_t sensei_commission_transcript(uint8_t *out, size_t cap, const uint8_t *ctx, size_t ctx_len, const uint8_t *id_p,
                                    size_t id_p_len, const uint8_t *id_v, size_t id_v_len,
                                    const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t z[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE]) {
  const size_t fixed = 10 * 8 + 6 * SENSEI_COMMISSION_POINT_SIZE + SENSEI_COMMISSION_SCALAR_SIZE;
  size_t pos = 0;

  /* the lengths come from the caller: compare without adding them up first (the sum could wrap) */
  if (cap < fixed || ctx_len > cap - fixed || id_p_len > cap - fixed - ctx_len ||
      id_v_len > cap - fixed - ctx_len - id_p_len) {
    return 0;
  }
  pos = tt_item(out, pos, ctx, ctx_len);
  pos = tt_item(out, pos, id_p, id_p_len);
  pos = tt_item(out, pos, id_v, id_v_len);
  pos = tt_item(out, pos, sensei_commission_m, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, sensei_commission_n, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, share_p, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, share_v, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, z, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, v, SENSEI_COMMISSION_POINT_SIZE);
  pos = tt_item(out, pos, w0, SENSEI_COMMISSION_SCALAR_SIZE);
  return pos;
}

int sensei_commission_schedule(const struct sensei_secure_crypto *c, const uint8_t *tt, size_t tt_len,
                               const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                               const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                               struct sensei_commission_schedule *out) {
  static const uint8_t no_salt[1] = {0};
  static const uint8_t lbl_conf[16] = {'C', 'o', 'n', 'f', 'i', 'r', 'm', 'a', 't', 'i', 'o', 'n', 'K', 'e', 'y', 's'};
  static const uint8_t lbl_shared[9] = {'S', 'h', 'a', 'r', 'e', 'd', 'K', 'e', 'y'};
  static const uint8_t lbl_sid[9] = {'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd'};
  uint8_t conf[64];
  int err = ERR_CRYPTO;

  if (c->sha256(c->ctx, tt, tt_len, out->k_main) != 0 ||
      c->hkdf(c->ctx, out->k_main, 32, no_salt, 0, lbl_conf, sizeof(lbl_conf), conf, sizeof(conf)) != 0 ||
      c->hkdf(c->ctx, out->k_main, 32, no_salt, 0, lbl_shared, sizeof(lbl_shared), out->k_shared, 32) != 0 ||
      c->hkdf(c->ctx, out->k_main, 32, no_salt, 0, lbl_sid, sizeof(lbl_sid), out->session_id,
              SENSEI_COMMISSION_SESSION_ID_SIZE) != 0) {
    goto out;
  }
  memcpy(out->k_confirm_p, conf, 32);
  memcpy(out->k_confirm_v, conf + 32, 32);
  if (c->hmac_sha256(c->ctx, out->k_confirm_p, 32, share_v, SENSEI_COMMISSION_POINT_SIZE, out->confirm_p) != 0 ||
      c->hmac_sha256(c->ctx, out->k_confirm_v, 32, share_p, SENSEI_COMMISSION_POINT_SIZE, out->confirm_v) != 0) {
    goto out;
  }
  err = 0;
out:
  wipe(conf, sizeof(conf));
  return err;
}

int sensei_commission_channel_keys(const struct sensei_secure_crypto *c, const uint8_t k_shared[32], uint64_t node_id,
                                   const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE], uint8_t k_c2n[16],
                                   uint8_t k_n2c[16], uint32_t *sid) {
  uint8_t info[sizeof(CHANNEL_INFO) + 8 + 2 * SENSEI_COMMISSION_NONCE_SIZE];
  uint8_t okm[36];
  int err = ERR_CRYPTO;

  memcpy(info, CHANNEL_INFO, sizeof(CHANNEL_INFO));
  put64be(info + sizeof(CHANNEL_INFO), node_id);
  memcpy(info + sizeof(CHANNEL_INFO) + 8, nonce_g, SENSEI_COMMISSION_NONCE_SIZE);
  memcpy(info + sizeof(CHANNEL_INFO) + 8 + SENSEI_COMMISSION_NONCE_SIZE, nonce_n, SENSEI_COMMISSION_NONCE_SIZE);
  if (c->hkdf(c->ctx, k_shared, 32, CONTEXT_LABEL, sizeof(CONTEXT_LABEL), info, sizeof(info), okm, sizeof(okm)) != 0 ||
      get32le(okm + 32) == 0) {
    goto out;
  }
  memcpy(k_c2n, okm, 16);
  memcpy(k_n2c, okm + 16, 16);
  *sid = get32le(okm + 32);
  err = 0;
out:
  wipe(okm, sizeof(okm));
  return err;
}

/* The keys of one session from the values of the exchange: transcript, schedule, channel keys. */
struct derived {
  uint8_t confirm_p[32], confirm_v[32];
  uint8_t k_c2n[16], k_n2c[16];
  uint8_t session_id[SENSEI_COMMISSION_SESSION_ID_SIZE];
  uint32_t sid;
};

static int derive(const struct sensei_secure_crypto *c, const uint8_t *ctx, const uint8_t *id_p, const uint8_t *id_v,
                  uint64_t node_id, const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                  const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE],
                  const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                  const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE], const uint8_t z[SENSEI_COMMISSION_POINT_SIZE],
                  const uint8_t v[SENSEI_COMMISSION_POINT_SIZE], const uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE],
                  struct derived *out) {
  uint8_t tt[SENSEI_COMMISSION_TT_SIZE];
  struct sensei_commission_schedule sch;
  size_t n;
  int err;

  n = sensei_commission_transcript(tt, sizeof(tt), ctx, SENSEI_COMMISSION_CONTEXT_SIZE, id_p, SENSEI_COMMISSION_ID_SIZE,
                                   id_v, SENSEI_COMMISSION_ID_SIZE, share_p, share_v, z, v, w0);
  err = n == SENSEI_COMMISSION_TT_SIZE ? sensei_commission_schedule(c, tt, n, share_p, share_v, &sch) : ERR_CRYPTO;
  wipe(tt, sizeof(tt));
  if (err == 0) {
    memcpy(out->confirm_p, sch.confirm_p, 32);
    memcpy(out->confirm_v, sch.confirm_v, 32);
    memcpy(out->session_id, sch.session_id, SENSEI_COMMISSION_SESSION_ID_SIZE);
    err = sensei_commission_channel_keys(c, sch.k_shared, node_id, nonce_g, nonce_n, out->k_c2n, out->k_n2c, &out->sid);
  }
  wipe(&sch, sizeof(sch));
  return err;
}

/* An ephemeral scalar: 40 random bytes reduced mod n, not 0. */
static int draw_scalar(const struct sensei_secure_crypto *c, uint8_t out[32]) {
  uint8_t r[40];

  for (int tries = 0; tries < 8; tries++) {
    int bad = c->random(c->ctx, r, sizeof(r)) != 0 || c->scalar_reduce(c->ctx, r, out) != 0;

    wipe(r, sizeof(r));
    if (bad) {
      return ERR_CRYPTO;
    }
    if (!is_zero(out, 32)) {
      return 0;
    }
  }
  return ERR_CRYPTO;
}

int sensei_commission_derive_w(const struct sensei_secure_crypto *c, const uint8_t dk[SENSEI_COMMISSION_DK_SIZE],
                               uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE], uint8_t w1[SENSEI_COMMISSION_SCALAR_SIZE]) {
  uint8_t a[32], b[32];
  int err = ERR_CRYPTO;

  if (!c->scalar_reduce || c->scalar_reduce(c->ctx, dk, a) != 0 || c->scalar_reduce(c->ctx, dk + 40, b) != 0) {
    goto out;
  }
  if (is_zero(a, 32) || is_zero(b, 32)) {
    err = ERR_FORMAT;
    goto out;
  }
  memcpy(w0, a, 32);
  memcpy(w1, b, 32);
  err = 0;
out:
  wipe(a, sizeof(a));
  wipe(b, sizeof(b));
  return err;
}

/* ---- the kind 5 channel ---- */

int sensei_commission_channel_open(struct sensei_commission_channel *ch, const struct sensei_secure_crypto *c,
                                   enum sensei_commission_role role, const uint8_t k_c2n[16], const uint8_t k_n2c[16],
                                   uint32_t sid) {
  const bool prover = role == SENSEI_COMMISSION_PROVER;

  sensei_commission_channel_close(ch); /* a channel that is open is closed first: its key handles must not leak */
  memset(ch, 0, sizeof(*ch));
  if (sid == 0) {
    return ERR_STATE;
  }
  if (c->aead_import(c->ctx, prover ? k_c2n : k_n2c, &ch->k_tx) != 0) {
    return ERR_CRYPTO;
  }
  if (c->aead_import(c->ctx, prover ? k_n2c : k_c2n, &ch->k_rx) != 0) {
    (void)c->key_destroy(c->ctx, ch->k_tx);
    ch->k_tx = 0;
    return ERR_CRYPTO;
  }
  ch->c = c;
  ch->sid = sid;
  ch->to_node = prover;
  ch->open = true;
  return 0;
}

int sensei_commission_channel_seal(struct sensei_commission_channel *ch, const uint8_t *pt, size_t len, uint8_t *out,
                                   size_t cap) {
  int n;

  if (!ch->open || ch->tx_counter == 0xFFFFFFFFu) {
    return ERR_STATE;
  }
  n = sensei_secure_seal_commission(ch->c, ch->k_tx, ch->sid, ch->to_node, ch->tx_counter + 1u, pt, len, out, cap);
  if (n > 0) {
    ch->tx_counter++;
  }
  return n;
}

int sensei_commission_channel_recv(struct sensei_commission_channel *ch, const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t cap, size_t *out_len) {
  if (!ch->open) {
    return ERR_STATE;
  }
  /* The prover sends to the node and so receives from the node; the node is the other way round. */
  return sensei_secure_open_commission(ch->c, ch->k_rx, ch->sid, ch->to_node, &ch->rx, in, in_len, out, cap, out_len);
}

void sensei_commission_channel_close(struct sensei_commission_channel *ch) {
  if (ch->open) {
    (void)ch->c->key_destroy(ch->c->ctx, ch->k_tx);
    (void)ch->c->key_destroy(ch->c->ctx, ch->k_rx);
  }
  wipe(ch, sizeof(*ch));
}

/* ---- verifier (the node) ---- */

static uint32_t limit_or(uint32_t v, uint32_t def) { return v ? v : def; }

void sensei_commission_verifier_end(struct sensei_commission_verifier *v) {
  sensei_commission_channel_close(&v->ch);
  wipe(v, sizeof(*v));
}

/* True if a time limit has passed. */
static bool v_expired(const struct sensei_commission_verifier *v, uint32_t now) {
  uint32_t lim;

  if (now - v->t_begin >= v->lim.session_ms) {
    return true;
  }
  switch (v->state) {
  case SENSEI_COMMISSION_BEGUN:
    lim = v->lim.begin_ms;
    break;
  case SENSEI_COMMISSION_SENT:
    lim = v->lim.pake_ms;
    break;
  default:
    lim = v->lim.idle_ms;
    break;
  }
  return now - v->t_phase >= lim;
}

/* The step functions: not idle, in time and in the state that the step needs. Else the session ends. */
static int v_enter(struct sensei_commission_verifier *v, uint32_t now, enum sensei_commission_state want) {
  int err = 0;

  if (v->state == SENSEI_COMMISSION_IDLE) {
    return ERR_STATE;
  }
  if (v_expired(v, now)) {
    err = SENSEI_COMMISSION_ERR_TIMEOUT;
  } else if (v->state != want) {
    err = ERR_STATE;
  }
  if (err) {
    sensei_commission_verifier_end(v);
  }
  return err;
}

int sensei_commission_verifier_poll(struct sensei_commission_verifier *v, uint32_t now_ms) {
  if (v->state != SENSEI_COMMISSION_IDLE && v_expired(v, now_ms)) {
    sensei_commission_verifier_end(v);
    return SENSEI_COMMISSION_ERR_TIMEOUT;
  }
  return 0;
}

enum sensei_commission_state sensei_commission_verifier_state(const struct sensei_commission_verifier *v) {
  return v->state;
}

uint32_t sensei_commission_verifier_sid(const struct sensei_commission_verifier *v) {
  return v->state == SENSEI_COMMISSION_OPEN ? v->ch.sid : 0u;
}

int sensei_commission_verifier_begin(struct sensei_commission_verifier *v,
                                     const struct sensei_commission_verifier_cfg *cfg,
                                     const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE], uint32_t now_ms,
                                     uint8_t nonce_n_out[SENSEI_COMMISSION_NONCE_SIZE]) {
  const struct sensei_secure_crypto *c;

  if (v->state != SENSEI_COMMISSION_IDLE) {
    sensei_commission_verifier_end(v); /* a session that is still running: destroy its key handles first */
  }
  memset(v, 0, sizeof(*v));
  if (!cfg || !nonce_g || !nonce_n_out || !cfg->record || !cfg->counters || !cfg->save_counters ||
      !backend_ok(cfg->c)) {
    return ERR_STATE;
  }
  c = cfg->c;
  if (cfg->node_id == 0 || record_check(c, cfg->record) != 0) {
    return ERR_FORMAT;
  }
  v->cfg = *cfg;
  v->lim.begin_ms = limit_or(cfg->limits.begin_ms, SENSEI_COMMISSION_BEGIN_MS);
  v->lim.pake_ms = limit_or(cfg->limits.pake_ms, SENSEI_COMMISSION_PAKE_MS);
  v->lim.idle_ms = limit_or(cfg->limits.idle_ms, SENSEI_COMMISSION_IDLE_MS);
  v->lim.session_ms = limit_or(cfg->limits.session_ms, SENSEI_COMMISSION_SESSION_MS);
  v->lim.fails_max = limit_or(cfg->limits.fails_max, SENSEI_COMMISSION_FAILS_MAX);
  if (cfg->counters->fails_since_ok >= v->lim.fails_max) {
    wipe(v, sizeof(*v));
    return SENSEI_COMMISSION_ERR_LOCKED;
  }
  if (c->random(c->ctx, v->nonce_n, sizeof(v->nonce_n)) != 0) {
    wipe(v, sizeof(*v));
    return ERR_CRYPTO;
  }
  v->rec = *cfg->record;
  memcpy(v->nonce_g, nonce_g, sizeof(v->nonce_g));
  memcpy(nonce_n_out, v->nonce_n, sizeof(v->nonce_n));
  v->t_begin = v->t_phase = now_ms;
  v->state = SENSEI_COMMISSION_BEGUN;
  return 0;
}

int sensei_commission_verifier_pake1(struct sensei_commission_verifier *v, uint32_t now_ms,
                                     const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                                     uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                     uint8_t confirm_v[SENSEI_COMMISSION_CONFIRM_SIZE]) {
  const struct sensei_secure_crypto *c;
  struct sensei_commission_counters *cnt;
  struct sensei_commission_counters next;
  uint8_t y[32], z[SENSEI_COMMISSION_POINT_SIZE], vv[SENSEI_COMMISSION_POINT_SIZE];
  uint8_t sh_v[SENSEI_COMMISSION_POINT_SIZE], ctx[SENSEI_COMMISSION_CONTEXT_SIZE], id_v[SENSEI_COMMISSION_ID_SIZE];
  struct derived d;
  int err;

  err = v_enter(v, now_ms, SENSEI_COMMISSION_BEGUN);
  if (err) {
    return err;
  }
  c = v->cfg.c;
  cnt = v->cfg.counters;
  memset(&d, 0, sizeof(d));
  if (cnt->fails_since_ok >= v->lim.fails_max) {
    err = SENSEI_COMMISSION_ERR_LOCKED;
    goto fail;
  }
  if (!share_p || !share_v || !confirm_v || share_p[0] != 0x04 || c->spake2p_check(c->ctx, share_p) != 0) {
    err = ERR_FORMAT; /* not a point of the curve: not an attempt */
    goto fail;
  }
  err = draw_scalar(c, y);
  if (err) {
    goto fail;
  }
  /* rest = shareP - w0*M, Z = y*rest, V = y*L. A share that is w0*M (rest is the identity) is refused here. */
  if (c->spake2p_zv(c->ctx, v->rec.w0, NULL, y, share_p, sensei_commission_m, v->rec.l, z, vv) != 0) {
    err = ERR_FORMAT; /* not an attempt either */
    goto fail;
  }
  if (c->spake2p_share(c->ctx, v->rec.w0, y, sensei_commission_n, sh_v) != 0) {
    err = ERR_CRYPTO;
    goto fail;
  }
  sensei_commission_context(&v->rec.params, v->nonce_g, v->nonce_n, ctx);
  sensei_commission_id_verifier(v->cfg.node_id, id_v);
  err = derive(c, ctx, ID_PROVER, id_v, v->cfg.node_id, v->nonce_g, v->nonce_n, share_p, sh_v, z, vv, v->rec.w0, &d);
  if (err) {
    goto fail;
  }
  /* The attempt counts from here: the flash write comes before shareV leaves, so that a reset or a power cut never
   * gives an attempt back. A failing write ends the session and nothing is sent. */
  next = *cnt;
  next.fails_total += next.fails_total != 0xFFFFFFFFu;
  next.fails_since_ok += next.fails_since_ok != 0xFFFFFFFFu;
  if (v->cfg.save_counters(v->cfg.user, &next) != 0) {
    err = ERR_STORE;
    goto fail;
  }
  *cnt = next;
  memcpy(share_v, sh_v, sizeof(sh_v));
  memcpy(confirm_v, d.confirm_v, sizeof(d.confirm_v));
  memcpy(v->confirm_p, d.confirm_p, sizeof(v->confirm_p));
  memcpy(v->k_c2n, d.k_c2n, sizeof(v->k_c2n));
  memcpy(v->k_n2c, d.k_n2c, sizeof(v->k_n2c));
  v->sid = d.sid;
  memcpy(v->session_id, d.session_id, sizeof(v->session_id));
  wipe(v->rec.w0, sizeof(v->rec.w0));
  wipe(v->rec.l, sizeof(v->rec.l));
  v->state = SENSEI_COMMISSION_SENT;
  v->t_phase = now_ms;
  err = 0;
  goto out;
fail:
  sensei_commission_verifier_end(v);
out:
  wipe(y, sizeof(y));
  wipe(z, sizeof(z));
  wipe(vv, sizeof(vv));
  wipe(sh_v, sizeof(sh_v));
  wipe(&d, sizeof(d));
  return err;
}

int sensei_commission_verifier_pake3(struct sensei_commission_verifier *v, uint32_t now_ms,
                                     const uint8_t confirm_p[SENSEI_COMMISSION_CONFIRM_SIZE]) {
  struct sensei_commission_counters next;
  const struct sensei_secure_crypto *c;
  int err = v_enter(v, now_ms, SENSEI_COMMISSION_SENT);

  if (err) {
    return err;
  }
  c = v->cfg.c;
  if (!confirm_p || !ct_equal(confirm_p, v->confirm_p, sizeof(v->confirm_p))) {
    sensei_commission_verifier_end(v); /* the attempt was counted at pake1 and stays counted */
    return ERR_AUTH;
  }
  err = sensei_commission_channel_open(&v->ch, c, SENSEI_COMMISSION_VERIFIER, v->k_c2n, v->k_n2c, v->sid);
  wipe(v->k_c2n, sizeof(v->k_c2n));
  wipe(v->k_n2c, sizeof(v->k_n2c));
  wipe(v->confirm_p, sizeof(v->confirm_p));
  if (err) {
    sensei_commission_verifier_end(v);
    return err;
  }
  /* Confirmed: the attempt was not a failure. The counters go to the confirmed state; if the write fails the success
   * stands and the counters stay as they were (one failure more than there was: the safe side). */
  next = *v->cfg.counters;
  next.fails_since_ok = 0;
  next.fails_total -= next.fails_total != 0;
  if (v->cfg.save_counters(v->cfg.user, &next) == 0) {
    *v->cfg.counters = next;
  }
  v->state = SENSEI_COMMISSION_OPEN;
  v->t_phase = now_ms;
  return 0;
}

int sensei_commission_verifier_seal(struct sensei_commission_verifier *v, uint32_t now_ms, const uint8_t *pt,
                                    size_t len, uint8_t *out, size_t cap) {
  int n;

  if (v->state == SENSEI_COMMISSION_IDLE) {
    return ERR_STATE;
  }
  if (v_expired(v, now_ms)) {
    sensei_commission_verifier_end(v);
    return SENSEI_COMMISSION_ERR_TIMEOUT;
  }
  if (v->state != SENSEI_COMMISSION_OPEN) {
    return ERR_STATE;
  }
  n = sensei_commission_channel_seal(&v->ch, pt, len, out, cap);
  if (n > 0) {
    v->t_phase = now_ms;
  }
  return n;
}

int sensei_commission_verifier_recv(struct sensei_commission_verifier *v, uint32_t now_ms, const uint8_t *in,
                                    size_t in_len, uint8_t *out, size_t cap, size_t *out_len) {
  int err;

  if (v->state == SENSEI_COMMISSION_IDLE) {
    return ERR_STATE;
  }
  if (v_expired(v, now_ms)) {
    sensei_commission_verifier_end(v);
    return SENSEI_COMMISSION_ERR_TIMEOUT;
  }
  if (v->state != SENSEI_COMMISSION_OPEN) {
    return ERR_STATE;
  }
  err = sensei_commission_channel_recv(&v->ch, in, in_len, out, cap, out_len);
  if (err == 0) {
    v->t_phase = now_ms;
  }
  return err;
}

/* ---- prover (the commissioner) ---- */

void sensei_commission_prover_end(struct sensei_commission_prover *p) {
  sensei_commission_channel_close(&p->ch);
  wipe(p, sizeof(*p));
}

int sensei_commission_prover_begin(struct sensei_commission_prover *p, const struct sensei_secure_crypto *c,
                                   const uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE],
                                   const uint8_t w1[SENSEI_COMMISSION_SCALAR_SIZE], uint64_t node_id,
                                   const struct sensei_commission_params *params,
                                   const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE]) {
  int err;

  if (p->state != SENSEI_COMMISSION_IDLE) {
    sensei_commission_prover_end(p);
  }
  memset(p, 0, sizeof(*p));
  if (!w0 || !w1 || !params || !nonce_g || !nonce_n || !backend_ok(c)) {
    return ERR_STATE;
  }
  if (node_id == 0 || sensei_commission_params_check(params) != 0 || !sensei_secure_priv_valid(w0) ||
      !sensei_secure_priv_valid(w1)) {
    return ERR_FORMAT;
  }
  p->c = c;
  p->params = *params;
  memcpy(p->w0, w0, sizeof(p->w0));
  memcpy(p->w1, w1, sizeof(p->w1));
  memcpy(p->nonce_g, nonce_g, sizeof(p->nonce_g));
  memcpy(p->nonce_n, nonce_n, sizeof(p->nonce_n));
  p->node_id = node_id;
  sensei_commission_context(params, nonce_g, nonce_n, p->ctx);
  sensei_commission_id_verifier(node_id, p->id_v);
  err = draw_scalar(c, p->x);
  if (err) {
    sensei_commission_prover_end(p);
    return err;
  }
  p->state = SENSEI_COMMISSION_BEGUN;
  return 0;
}

int sensei_commission_prover_share(struct sensei_commission_prover *p, uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE]) {
  if (p->state != SENSEI_COMMISSION_BEGUN) {
    return ERR_STATE;
  }
  if (p->c->spake2p_share(p->c->ctx, p->w0, p->x, sensei_commission_m, p->share_p) != 0) {
    sensei_commission_prover_end(p);
    return ERR_CRYPTO;
  }
  memcpy(share_p, p->share_p, sizeof(p->share_p));
  p->state = SENSEI_COMMISSION_SENT;
  return 0;
}

int sensei_commission_prover_finish(struct sensei_commission_prover *p,
                                    const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t confirm_v[SENSEI_COMMISSION_CONFIRM_SIZE],
                                    uint8_t confirm_p[SENSEI_COMMISSION_CONFIRM_SIZE]) {
  const struct sensei_secure_crypto *c = p->c;
  uint8_t z[SENSEI_COMMISSION_POINT_SIZE], vv[SENSEI_COMMISSION_POINT_SIZE];
  struct derived d;
  int err;

  if (p->state != SENSEI_COMMISSION_SENT) {
    return ERR_STATE;
  }
  memset(&d, 0, sizeof(d));
  if (!share_v || !confirm_v || !confirm_p || share_v[0] != 0x04 || c->spake2p_check(c->ctx, share_v) != 0 ||
      c->spake2p_zv(c->ctx, p->w0, p->w1, p->x, share_v, sensei_commission_n, NULL, z, vv) != 0) {
    err = ERR_FORMAT;
    goto fail;
  }
  err =
      derive(c, p->ctx, ID_PROVER, p->id_v, p->node_id, p->nonce_g, p->nonce_n, p->share_p, share_v, z, vv, p->w0, &d);
  if (err) {
    goto fail;
  }
  if (!ct_equal(confirm_v, d.confirm_v, sizeof(d.confirm_v))) {
    err = ERR_AUTH;
    goto fail;
  }
  err = sensei_commission_channel_open(&p->ch, c, SENSEI_COMMISSION_PROVER, d.k_c2n, d.k_n2c, d.sid);
  if (err) {
    goto fail;
  }
  memcpy(confirm_p, d.confirm_p, sizeof(d.confirm_p));
  memcpy(p->session_id, d.session_id, sizeof(p->session_id));
  wipe(p->w0, sizeof(p->w0));
  wipe(p->w1, sizeof(p->w1));
  wipe(p->x, sizeof(p->x));
  p->state = SENSEI_COMMISSION_OPEN;
  goto out;
fail:
  sensei_commission_prover_end(p);
out:
  wipe(z, sizeof(z));
  wipe(vv, sizeof(vv));
  wipe(&d, sizeof(d));
  return err;
}

int sensei_commission_prover_seal(struct sensei_commission_prover *p, const uint8_t *pt, size_t len, uint8_t *out,
                                  size_t cap) {
  return p->state == SENSEI_COMMISSION_OPEN ? sensei_commission_channel_seal(&p->ch, pt, len, out, cap) : ERR_STATE;
}

int sensei_commission_prover_recv(struct sensei_commission_prover *p, const uint8_t *in, size_t in_len, uint8_t *out,
                                  size_t cap, size_t *out_len) {
  return p->state == SENSEI_COMMISSION_OPEN ? sensei_commission_channel_recv(&p->ch, in, in_len, out, cap, out_len)
                                            : ERR_STATE;
}

/* ---- fingerprints and the proof of possession ---- */

int sensei_commission_fingerprint(const struct sensei_secure_crypto *c, uint64_t node_id,
                                  const uint8_t node_pub[SENSEI_SECURE_PUB_SIZE],
                                  uint8_t out[SENSEI_COMMISSION_FP_SIZE]) {
  uint8_t buf[sizeof(FP_NODE_LABEL) + 8 + SENSEI_SECURE_PUB_SIZE];
  uint8_t h[32];

  if (!c->sha256) {
    return ERR_CRYPTO;
  }
  memcpy(buf, FP_NODE_LABEL, sizeof(FP_NODE_LABEL));
  put64be(buf + sizeof(FP_NODE_LABEL), node_id);
  memcpy(buf + sizeof(FP_NODE_LABEL) + 8, node_pub, SENSEI_SECURE_PUB_SIZE);
  if (c->sha256(c->ctx, buf, sizeof(buf), h) != 0) {
    return ERR_CRYPTO;
  }
  memcpy(out, h, SENSEI_COMMISSION_FP_SIZE);
  return 0;
}

int sensei_commission_collector_fingerprint(const struct sensei_secure_crypto *c,
                                            const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE],
                                            uint8_t out[SENSEI_COMMISSION_COLLECTOR_FP_SIZE]) {
  uint8_t buf[sizeof(FP_COLLECTOR_LABEL) + SENSEI_SECURE_PUB_SIZE];
  uint8_t h[32];

  if (!c->sha256) {
    return ERR_CRYPTO;
  }
  memcpy(buf, FP_COLLECTOR_LABEL, sizeof(FP_COLLECTOR_LABEL));
  memcpy(buf + sizeof(FP_COLLECTOR_LABEL), backend_pub, SENSEI_SECURE_PUB_SIZE);
  if (c->sha256(c->ctx, buf, sizeof(buf), h) != 0) {
    return ERR_CRYPTO;
  }
  memcpy(out, h, SENSEI_COMMISSION_COLLECTOR_FP_SIZE);
  return 0;
}

void sensei_commission_proof_plain(uint64_t node_id, const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t session_id[SENSEI_COMMISSION_SESSION_ID_SIZE],
                                   uint8_t out[SENSEI_COMMISSION_POP_SIZE]) {
  memcpy(out, POP_LABEL, sizeof(POP_LABEL));
  put64be(out + sizeof(POP_LABEL), node_id);
  memcpy(out + sizeof(POP_LABEL) + 8, nonce_g, SENSEI_COMMISSION_NONCE_SIZE);
  memcpy(out + sizeof(POP_LABEL) + 8 + SENSEI_COMMISSION_NONCE_SIZE, nonce_n, SENSEI_COMMISSION_NONCE_SIZE);
  memcpy(out + sizeof(POP_LABEL) + 8 + 2 * SENSEI_COMMISSION_NONCE_SIZE, session_id, SENSEI_COMMISSION_SESSION_ID_SIZE);
}

/* ---- setup code, QR payload, label ---- */

/* Verhoeff tables (dihedral group D5), as in sensei_sdk/commission.py. */
static const uint8_t VD[10][10] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 2, 3, 4, 0, 6, 7, 8, 9, 5},
                                   {2, 3, 4, 0, 1, 7, 8, 9, 5, 6}, {3, 4, 0, 1, 2, 8, 9, 5, 6, 7},
                                   {4, 0, 1, 2, 3, 9, 5, 6, 7, 8}, {5, 9, 8, 7, 6, 0, 4, 3, 2, 1},
                                   {6, 5, 9, 8, 7, 1, 0, 4, 3, 2}, {7, 6, 5, 9, 8, 2, 1, 0, 4, 3},
                                   {8, 7, 6, 5, 9, 3, 2, 1, 0, 4}, {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
static const uint8_t VP[8][10] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 5, 7, 6, 2, 8, 3, 0, 9, 4},
                                  {5, 8, 0, 3, 7, 9, 6, 1, 4, 2}, {8, 9, 1, 6, 0, 4, 3, 5, 2, 7},
                                  {9, 4, 5, 3, 1, 2, 6, 8, 7, 0}, {4, 2, 8, 6, 5, 7, 3, 9, 0, 1},
                                  {2, 7, 9, 3, 8, 0, 6, 4, 1, 5}, {7, 0, 4, 6, 9, 1, 3, 2, 5, 8}};
static const uint8_t VINV[10] = {0, 4, 3, 2, 1, 5, 6, 7, 8, 9};

static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

int sensei_commission_verhoeff_digit(const char *digits, size_t n) {
  unsigned c = 0;

  for (size_t i = 0; i < n; i++) {
    char ch = digits[n - 1 - i];

    if (!is_digit(ch)) {
      return -1;
    }
    c = VD[c][VP[(i + 1) % 8][ch - '0']];
  }
  return VINV[c];
}

/* True if the last of the n digits is the check digit of the others. */
static bool verhoeff_valid(const char *digits, size_t n) {
  unsigned c = 0;

  for (size_t i = 0; i < n; i++) {
    c = VD[c][VP[i % 8][digits[n - 1 - i] - '0']];
  }
  return c == 0;
}

int sensei_commission_code_parse(const char *text, char out[SENSEI_COMMISSION_CODE_SIZE + 1]) {
  char code[SENSEI_COMMISSION_CODE_SIZE + 1];
  size_t n = 0;

  if (!text) {
    return ERR_FORMAT;
  }
  for (; *text; text++) {
    if (*text == '-' || *text == ' ' || (*text >= '\t' && *text <= '\r')) {
      continue;
    }
    if (!is_digit(*text) || n == SENSEI_COMMISSION_CODE_SIZE) {
      return ERR_FORMAT;
    }
    code[n++] = *text;
  }
  if (n != SENSEI_COMMISSION_CODE_SIZE || !verhoeff_valid(code, n)) {
    return ERR_FORMAT;
  }
  memcpy(out, code, n);
  out[n] = 0;
  return 0;
}

void sensei_commission_code_format(const char code[SENSEI_COMMISSION_CODE_SIZE], char out[15]) {
  size_t o = 0;

  for (size_t i = 0; i < SENSEI_COMMISSION_CODE_SIZE; i++) {
    if (i && i % 4 == 0) {
      out[o++] = '-';
    }
    out[o++] = code[i];
  }
  out[o] = 0;
}

int sensei_commission_pwin(const char *code, uint64_t node_id, uint8_t *out, size_t cap) {
  char c12[SENSEI_COMMISSION_CODE_SIZE + 1];
  size_t pos = 0;

  if (!out || sensei_commission_code_parse(code, c12) != 0 ||
      cap < 3 * 8 + SENSEI_COMMISSION_CODE_DIGITS + 2 * SENSEI_COMMISSION_ID_SIZE) {
    return ERR_FORMAT;
  }
  put64le(out + pos, SENSEI_COMMISSION_CODE_DIGITS);
  memcpy(out + pos + 8, c12, SENSEI_COMMISSION_CODE_DIGITS);
  pos += 8 + SENSEI_COMMISSION_CODE_DIGITS;
  pos = tt_item(out, pos, ID_PROVER, SENSEI_COMMISSION_ID_SIZE);
  put64le(out + pos, SENSEI_COMMISSION_ID_SIZE);
  sensei_commission_id_verifier(node_id, out + pos + 8);
  pos += 8 + SENSEI_COMMISSION_ID_SIZE;
  return (int)pos;
}

static const char HEX_UP[] = "0123456789ABCDEF";
static const char HEX_LOW[] = "0123456789abcdef";

static size_t put_hex(char *out, const uint8_t *data, size_t n, const char *digits) {
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 15];
  }
  return 2 * n;
}

static void put_id(char *out, uint64_t node_id) {
  uint8_t be[8];

  put64be(be, node_id);
  (void)put_hex(out, be, 8, HEX_UP);
}

int sensei_commission_qr_format(uint64_t node_id, const char *code, const uint8_t fp[SENSEI_COMMISSION_FP_SIZE],
                                char out[SENSEI_COMMISSION_QR_SIZE + 1]) {
  char c12[SENSEI_COMMISSION_CODE_SIZE + 1];
  size_t o = 0;

  if (node_id == 0 || sensei_commission_code_parse(code, c12) != 0 || strlen(code) != SENSEI_COMMISSION_CODE_SIZE) {
    return ERR_FORMAT;
  }
  memcpy(out, "SN1:", 4);
  put_id(out + 4, node_id);
  o = 4 + 16;
  out[o++] = ':';
  memcpy(out + o, c12, SENSEI_COMMISSION_CODE_SIZE);
  o += SENSEI_COMMISSION_CODE_SIZE;
  out[o++] = ':';
  o += put_hex(out + o, fp, SENSEI_COMMISSION_FP_SIZE, HEX_UP);
  out[o] = 0;
  return 0;
}

static int hex_val(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  return ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

int sensei_commission_qr_parse(const char *text, uint64_t *node_id, char code[SENSEI_COMMISSION_CODE_SIZE + 1],
                               uint8_t fp[SENSEI_COMMISSION_FP_SIZE]) {
  const char *p, *end;
  uint64_t id = 0;
  uint8_t f[SENSEI_COMMISSION_FP_SIZE];
  char c12[SENSEI_COMMISSION_CODE_SIZE + 1];

  if (!text) {
    return ERR_FORMAT;
  }
  while (*text == ' ' || (*text >= '\t' && *text <= '\r')) {
    text++;
  }
  end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || (end[-1] >= '\t' && end[-1] <= '\r'))) {
    end--;
  }
  if ((size_t)(end - text) != SENSEI_COMMISSION_QR_SIZE || memcmp(text, "SN1:", 4) != 0) {
    return ERR_FORMAT;
  }
  p = text + 4;
  for (int i = 0; i < 16; i++) {
    int h = hex_val(p[i]);

    if (h < 0) {
      return ERR_FORMAT;
    }
    id = (id << 4) | (uint64_t)h;
  }
  p += 16;
  if (*p++ != ':') {
    return ERR_FORMAT;
  }
  for (size_t i = 0; i < SENSEI_COMMISSION_CODE_SIZE; i++) {
    if (!is_digit(p[i])) {
      return ERR_FORMAT;
    }
    c12[i] = p[i];
  }
  c12[SENSEI_COMMISSION_CODE_SIZE] = 0;
  p += SENSEI_COMMISSION_CODE_SIZE;
  if (*p++ != ':') {
    return ERR_FORMAT;
  }
  for (size_t i = 0; i < SENSEI_COMMISSION_FP_SIZE; i++) {
    int hi = hex_val(p[2 * i]), lo = hex_val(p[2 * i + 1]);

    if (hi < 0 || lo < 0) {
      return ERR_FORMAT;
    }
    f[i] = (uint8_t)((hi << 4) | lo);
  }
  if (id == 0 || sensei_commission_code_parse(c12, c12) != 0) {
    return ERR_FORMAT;
  }
  *node_id = id;
  memcpy(code, c12, sizeof(c12));
  memcpy(fp, f, sizeof(f));
  return 0;
}

void sensei_commission_label_lines(uint64_t node_id, const char code[SENSEI_COMMISSION_CODE_SIZE],
                                   const uint8_t fp[SENSEI_COMMISSION_FP_SIZE], char line_id[25], char line_code[26],
                                   char line_key[24]) {
  char nid[16];
  size_t o;

  put_id(nid, node_id);
  memcpy(line_id, "SENSEI ", 7);
  memcpy(line_id + 7, nid, 8);
  line_id[15] = ' ';
  memcpy(line_id + 16, nid + 8, 8);
  line_id[24] = 0;
  memcpy(line_code, "Setup code ", 11);
  sensei_commission_code_format(code, line_code + 11);
  memcpy(line_key, "Key ", 4);
  o = 4;
  for (size_t i = 0; i < 4; i++) {
    if (i) {
      line_key[o++] = ' ';
    }
    o += put_hex(line_key + o, fp + 2 * i, 2, HEX_LOW);
  }
  line_key[o] = 0;
}
