/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Commissioning with the OpenSSL backend: the vectors V9 to V15 of the Python reference (sensei_sdk.commission), the
 * sessions of verifier and prover with fixed random numbers, aborts at every step, tampering, out of order calls, time
 * limits (the time is an input), attempt counting with a store that fails, wiping, the verifier record, the kind 5
 * channel and its refusal by the normal receivers. */

#include <string.h>

#include "check.h"
#include "sensei_commission.h"
#include "sensei_secure.h"
#include "vectors.h"
#include "vectors_commission.h"

static const struct sensei_secure_crypto *C0;

#define ARR(x) (x), sizeof(x)

/* ---- a backend whose random() returns queued bytes, and that can be made to fail ---- */

struct rng {
  uint8_t buf[1024];
  size_t len, pos;
  int fail_random;                    /* random() fails from this call on (1 = the next one), 0 = never */
  int calls;                          /* random() calls */
  int fail_share, fail_hkdf, fail_zv; /* the next call of this function fails */
};

static struct rng R;
static struct sensei_secure_crypto CT;

static int t_random(void *ctx, uint8_t *out, size_t len) {
  struct rng *r = &R;

  (void)ctx;
  r->calls++;
  if (r->fail_random && r->calls >= r->fail_random) {
    return -1;
  }
  if (r->pos + len > r->len) {
    return -1; /* a test asked for more random bytes than it queued */
  }
  memcpy(out, r->buf + r->pos, len);
  r->pos += len;
  return 0;
}

static int t_share(void *ctx, const uint8_t *w0, const uint8_t *xy, const uint8_t *mn, uint8_t *share) {
  struct rng *r = &R;

  (void)ctx;
  if (r->fail_share) {
    r->fail_share = 0;
    return -1;
  }
  return C0->spake2p_share(C0->ctx, w0, xy, mn, share);
}

static int t_hkdf(void *ctx, const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                  const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  struct rng *r = &R;

  (void)ctx;
  if (r->fail_hkdf) {
    r->fail_hkdf = 0;
    return -1;
  }
  return C0->hkdf(C0->ctx, ikm, ikm_len, salt, salt_len, info, info_len, okm, okm_len);
}

static int t_zv(void *ctx, const uint8_t *w0, const uint8_t *w1, const uint8_t *xy, const uint8_t *peer,
                const uint8_t *nm, const uint8_t *l, uint8_t *z, uint8_t *v) {
  struct rng *r = &R;

  (void)ctx;
  if (r->fail_zv) {
    r->fail_zv = 0;
    return -1;
  }
  return C0->spake2p_zv(C0->ctx, w0, w1, xy, peer, nm, l, z, v);
}

static void rng_reset(void) { memset(&R, 0, sizeof(R)); }

static void rng_put(const uint8_t *p, size_t n) {
  CHECK(R.len + n <= sizeof(R.buf));
  memcpy(R.buf + R.len, p, n);
  R.len += n;
}

/* A 40 byte draw that reduces to the 32 byte scalar k: 8 zero bytes, then k. */
static void rng_put_scalar(const uint8_t *k) {
  static const uint8_t zero8[8];

  rng_put(zero8, 8);
  rng_put(k, 32);
}

static void ct_init(void) {
  CT = *C0;
  CT.ctx = &R;
  CT.random = t_random;
  CT.spake2p_share = t_share;
  CT.hkdf = t_hkdf;
  CT.spake2p_zv = t_zv;
  rng_reset();
}

/* ---- counters in a fake flash: a write that fails changes nothing ---- */

struct flash {
  struct sensei_commission_counters stored;
  int writes;
  int fail; /* the next write fails */
  int fail_always;
};

static int flash_save(void *user, const struct sensei_commission_counters *cnt) {
  struct flash *f = user;

  if (f->fail || f->fail_always) {
    f->fail = 0;
    return -5;
  }
  f->stored = *cnt;
  f->writes++;
  return 0;
}

static struct flash F;
static struct sensei_commission_counters CNT;

static void flash_reset(uint32_t total, uint32_t since_ok) {
  memset(&F, 0, sizeof(F));
  F.stored.fails_total = total;
  F.stored.fails_since_ok = since_ok;
  CNT = F.stored;
}

static bool counters_are(uint32_t total, uint32_t since_ok) {
  return CNT.fails_total == total && CNT.fails_since_ok == since_ok && F.stored.fails_total == total &&
         F.stored.fails_since_ok == since_ok;
}

static bool is_zeros(const void *p, size_t n) {
  const uint8_t *b = p;

  while (n--) {
    if (*b++) {
      return false;
    }
  }
  return true;
}

/* ---- helpers on the vectors ---- */

static void mul_g(const uint8_t k[32], uint8_t pt[65]) {
  sensei_secure_key_t h = 0;

  CHECK(C0->ecdh_import(C0->ctx, k, 32, &h) == 0);
  CHECK(C0->ecdh_public(C0->ctx, h, pt) == 0);
  C0->key_destroy(C0->ctx, h);
}

static void record_of(int reg, struct sensei_commission_record *rec) {
  CHECK(sensei_commission_record_decode(C0, vec_com_reg[reg].record, SENSEI_COMMISSION_RECORD_SIZE, rec) == 0);
}

static struct sensei_commission_verifier_cfg cfg_for(const struct sensei_commission_record *rec, uint64_t node_id) {
  struct sensei_commission_verifier_cfg cfg;

  memset(&cfg, 0, sizeof(cfg));
  cfg.c = &CT;
  cfg.record = rec;
  cfg.node_id = node_id;
  cfg.counters = &CNT;
  cfg.save_counters = flash_save;
  cfg.user = &F;
  return cfg;
}

/* Queue the random bytes of session vector s: nonce_n, then (prover) x, then (verifier) y. */
static void queue_session(const vec_com_ses_t *s) {
  rng_reset();
  rng_put(s->nonce_n, 16);
  rng_put_scalar(s->x);
  rng_put_scalar(s->y);
}

/* The verifier alone: nonce_n and y. */
static void queue_verifier(const vec_com_ses_t *s) {
  rng_reset();
  rng_put(s->nonce_n, 16);
  rng_put_scalar(s->y);
}

/* ---- V9: RFC 9383 appendix C ---- */

static void test_v9(void) {
  uint8_t pt[65], pt2[65], z[65], v[65], tt[640];
  struct sensei_commission_schedule sch;
  uint8_t out32[32];
  size_t n;

  CHECK(memcmp(sensei_commission_m, vec_com_m, 65) == 0 && memcmp(sensei_commission_n, vec_com_n, 65) == 0);
  CHECK(C0->spake2p_check(C0->ctx, sensei_commission_m) == 0 && C0->spake2p_check(C0->ctx, sensei_commission_n) == 0);
  for (int i = 0; i < VEC_COM_MUL_COUNT; i++) {
    mul_g(vec_com_mul[i].k, pt);
    CHECK(memcmp(pt, vec_com_mul[i].point, 65) == 0);
    CHECK(C0->spake2p_check(C0->ctx, pt) == 0);
  }
  for (int i = 0; i < VEC_COM_RED_COUNT; i++) {
    CHECK(C0->scalar_reduce(C0->ctx, vec_com_red[i].in, out32) == 0);
    CHECK(memcmp(out32, vec_com_red[i].out, 32) == 0);
  }
  for (int i = 0; i < VEC_COM_BAD_COUNT; i++) {
    uint8_t buf[65];

    if (vec_com_bad[i].len == 65) { /* the other lengths are refused by the caller: the arguments are 65 B arrays */
      memcpy(buf, vec_com_bad[i].data, 65);
      CHECK(C0->spake2p_check(C0->ctx, buf) != 0);
    }
  }

  /* the exchange of the RFC: shares, Z, V on both sides, transcript, schedule */
  CHECK(C0->spake2p_share(C0->ctx, vec_com_rfc_w0, vec_com_rfc_x, sensei_commission_m, pt) == 0);
  CHECK(memcmp(pt, vec_com_rfc_share_p, 65) == 0);
  CHECK(C0->spake2p_share(C0->ctx, vec_com_rfc_w0, vec_com_rfc_y, sensei_commission_n, pt2) == 0);
  CHECK(memcmp(pt2, vec_com_rfc_share_v, 65) == 0);
  mul_g(vec_com_rfc_w1, pt);
  CHECK(memcmp(pt, vec_com_rfc_l, 65) == 0);
  /* prover: peer = shareV, nm = N, w1 */
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_x, vec_com_rfc_share_v, sensei_commission_n,
                       NULL, z, v) == 0);
  CHECK(memcmp(z, vec_com_rfc_z, 65) == 0 && memcmp(v, vec_com_rfc_v, 65) == 0);
  /* verifier: peer = shareP, nm = M, L */
  memset(z, 0, 65);
  memset(v, 0, 65);
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, sensei_commission_m,
                       vec_com_rfc_l, z, v) == 0);
  CHECK(memcmp(z, vec_com_rfc_z, 65) == 0 && memcmp(v, vec_com_rfc_v, 65) == 0);
  /* both w1 and L, or neither: refused */
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_y, vec_com_rfc_share_p, sensei_commission_m,
                       vec_com_rfc_l, z, v) != 0);
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, sensei_commission_m, NULL, z,
                       v) != 0);
  /* the same shares at the wrong element give other values (the roles are not symmetric) */
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, sensei_commission_n,
                       vec_com_rfc_l, z, v) != 0 ||
        memcmp(z, vec_com_rfc_z, 65) != 0);

  n = sensei_commission_transcript(tt, sizeof(tt), ARR(vec_com_rfc_context), ARR(vec_com_rfc_id_prover),
                                   ARR(vec_com_rfc_id_verifier), vec_com_rfc_share_p, vec_com_rfc_share_v,
                                   vec_com_rfc_z, vec_com_rfc_v, vec_com_rfc_w0);
  CHECK(n == VEC_COM_RFC_TT_LEN && memcmp(tt, vec_com_rfc_tt, n) == 0);
  CHECK(sensei_commission_transcript(tt, n - 1, ARR(vec_com_rfc_context), ARR(vec_com_rfc_id_prover),
                                     ARR(vec_com_rfc_id_verifier), vec_com_rfc_share_p, vec_com_rfc_share_v,
                                     vec_com_rfc_z, vec_com_rfc_v, vec_com_rfc_w0) == 0);
  CHECK(sensei_commission_schedule(C0, tt, n, vec_com_rfc_share_p, vec_com_rfc_share_v, &sch) == 0);
  CHECK(memcmp(sch.k_main, vec_com_rfc_k_main, 32) == 0);
  CHECK(memcmp(sch.k_confirm_p, vec_com_rfc_k_confirm_p, 32) == 0);
  CHECK(memcmp(sch.k_confirm_v, vec_com_rfc_k_confirm_v, 32) == 0);
  CHECK(memcmp(sch.confirm_p, vec_com_rfc_confirm_p, 32) == 0);
  CHECK(memcmp(sch.confirm_v, vec_com_rfc_confirm_v, 32) == 0);
  CHECK(memcmp(sch.k_shared, vec_com_rfc_k_shared, 32) == 0);
}

/* shares that the backend must refuse: not on the curve, and w0*M for the verifier (V15b is the session level) */
static void test_points(void) {
  uint8_t z[65], v[65], w0m[65];
  uint8_t one[32] = {0};

  for (int i = 0; i < VEC_COM_BAD_COUNT; i++) {
    uint8_t buf[65] = {0};

    if (vec_com_bad[i].len != 65) {
      continue;
    }
    memcpy(buf, vec_com_bad[i].data, 65);
    CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, buf, sensei_commission_m, vec_com_rfc_l, z, v) !=
          0);
    CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_y, buf, sensei_commission_n, NULL, z,
                         v) != 0);
    CHECK(C0->spake2p_share(C0->ctx, vec_com_rfc_w0, vec_com_rfc_y, buf, z) != 0);
  }
  /* x = 0 gives w0*M: a point, and exactly the share that a verifier must refuse (V15b is the session level) */
  CHECK(C0->spake2p_share(C0->ctx, vec_com_rfc_w0, one, sensei_commission_m, w0m) == 0);
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, w0m, sensei_commission_m, vec_com_rfc_l, z, v) !=
        0);
  one[31] = 1;
  CHECK(C0->spake2p_share(C0->ctx, vec_com_rfc_w0, one, sensei_commission_m, w0m) == 0);
  CHECK(C0->spake2p_zv(C0->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, w0m, sensei_commission_m, vec_com_rfc_l, z, v) ==
        0);
}

/* ---- V10: registration ---- */

static void test_v10(void) {
  for (int i = 0; i < VEC_COM_REG_COUNT; i++) {
    const vec_com_reg_t *r = &vec_com_reg[i];
    uint8_t pwin[80], w0[32], w1[32], l[65], rec[SENSEI_COMMISSION_RECORD_SIZE];
    struct sensei_commission_record rc;

    CHECK(sensei_commission_pwin(r->code, r->node_id, pwin, sizeof(pwin)) == (int)r->pwin_len);
    CHECK(memcmp(pwin, r->pwin, r->pwin_len) == 0);
    CHECK(sensei_commission_pwin(r->code, r->node_id, pwin, 72) < 0);
    CHECK(sensei_commission_pwin("000000000004", r->node_id, pwin, sizeof(pwin)) < 0);
    CHECK(sensei_commission_derive_w(C0, r->dk, w0, w1) == 0);
    CHECK(memcmp(w0, r->w0, 32) == 0 && memcmp(w1, r->w1, 32) == 0);
    mul_g(w1, l);
    CHECK(memcmp(l, r->l, 65) == 0);

    CHECK(sensei_commission_record_decode(C0, r->record, SENSEI_COMMISSION_RECORD_SIZE, &rc) == 0);
    CHECK(rc.params.suite == 1 && rc.params.kdf == 1 && rc.params.log2n == r->log2n && rc.params.r == 8 &&
          rc.params.p == 1);
    CHECK(memcmp(rc.params.salt, r->salt, 16) == 0 && memcmp(rc.w0, r->w0, 32) == 0 && memcmp(rc.l, r->l, 65) == 0);
    CHECK(sensei_commission_record_encode(C0, &rc, rec) == 0 && memcmp(rec, r->record, sizeof(rec)) == 0);
  }
  /* a scrypt output with a zero half is not accepted */
  {
    uint8_t dk[80], w0[32] = {1}, w1[32] = {2};

    memset(dk, 0, sizeof(dk));
    memset(dk + 40, 1, 40);
    CHECK(sensei_commission_derive_w(C0, dk, w0, w1) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(w0[0] == 1 && w1[0] == 2); /* nothing written */
    memset(dk, 1, 40);
    memset(dk + 40, 0, 40);
    CHECK(sensei_commission_derive_w(C0, dk, w0, w1) == SENSEI_SECURE_ERR_FORMAT);
  }
}

/* ---- V11: sessions with fixed ephemeral scalars ---- */

static void test_v11(void) {
  for (int i = 0; i < VEC_COM_SES_COUNT; i++) {
    const vec_com_ses_t *s = &vec_com_ses[i];
    const vec_com_reg_t *r = &vec_com_reg[s->reg];
    struct sensei_commission_params params;
    struct sensei_commission_record rec;
    struct sensei_commission_schedule sch;
    uint8_t ctx[73], idv[19], z[65], v[65], tt[SENSEI_COMMISSION_TT_SIZE];
    uint8_t k_c2n[16], k_n2c[16], nonce_n[16], share_p[65], share_v[65], confirm_v[32], confirm_p[32], pt[65];
    uint32_t sid = 0;
    struct sensei_commission_verifier ver;
    struct sensei_commission_prover pro;

    record_of(s->reg, &rec);
    params = rec.params;
    /* the pieces */
    sensei_commission_context(&params, s->nonce_g, s->nonce_n, ctx);
    CHECK(memcmp(ctx, s->context, 73) == 0);
    mul_g(s->x, pt);
    CHECK(C0->spake2p_share(C0->ctx, r->w0, s->x, sensei_commission_m, share_p) == 0);
    CHECK(memcmp(share_p, s->share_p, 65) == 0);
    CHECK(C0->spake2p_share(C0->ctx, r->w0, s->y, sensei_commission_n, share_v) == 0);
    CHECK(memcmp(share_v, s->share_v, 65) == 0);
    CHECK(C0->spake2p_zv(C0->ctx, r->w0, r->w1, s->x, s->share_v, sensei_commission_n, NULL, z, v) == 0);
    CHECK(memcmp(z, s->z, 65) == 0 && memcmp(v, s->v, 65) == 0);
    CHECK(C0->spake2p_zv(C0->ctx, r->w0, NULL, s->y, s->share_p, sensei_commission_m, r->l, z, v) == 0);
    CHECK(memcmp(z, s->z, 65) == 0 && memcmp(v, s->v, 65) == 0);
    sensei_commission_id_verifier(s->node_id, idv);
    CHECK(sensei_commission_transcript(tt, sizeof(tt), ctx, 73, (const uint8_t *)"SENSEI-commissioner", 19, idv, 19,
                                       s->share_p, s->share_v, s->z, s->v, r->w0) == s->tt_len);
    CHECK(memcmp(tt, s->tt, s->tt_len) == 0);
    CHECK(sensei_commission_schedule(C0, tt, s->tt_len, s->share_p, s->share_v, &sch) == 0);
    CHECK(memcmp(sch.k_main, s->k_main, 32) == 0 && memcmp(sch.k_confirm_p, s->k_confirm_p, 32) == 0 &&
          memcmp(sch.k_confirm_v, s->k_confirm_v, 32) == 0 && memcmp(sch.confirm_p, s->confirm_p, 32) == 0 &&
          memcmp(sch.confirm_v, s->confirm_v, 32) == 0 && memcmp(sch.k_shared, s->k_shared, 32) == 0);
    CHECK(sensei_commission_channel_keys(C0, sch.k_shared, s->node_id, s->nonce_g, s->nonce_n, k_c2n, k_n2c, &sid) ==
          0);
    CHECK(memcmp(k_c2n, s->k_c2n, 16) == 0 && memcmp(k_n2c, s->k_n2c, 16) == 0 && sid == s->sid);

    /* the sessions of the library, with the same random numbers */
    {
      struct sensei_commission_verifier_cfg cfg;
      uint8_t w0[32], w1[32];
      uint8_t frame[64], frame2[64];
      int n, n2;

      flash_reset(0, 0);
      ct_init();
      queue_session(s);
      cfg = cfg_for(&rec, s->node_id);
      CHECK(sensei_commission_verifier_begin(&ver, &cfg, s->nonce_g, 1000, nonce_n) == 0);
      CHECK(memcmp(nonce_n, s->nonce_n, 16) == 0);
      CHECK(sensei_commission_verifier_state(&ver) == SENSEI_COMMISSION_BEGUN);
      memcpy(w0, r->w0, 32);
      memcpy(w1, r->w1, 32);
      CHECK(sensei_commission_prover_begin(&pro, &CT, w0, w1, s->node_id, &params, s->nonce_g, nonce_n) == 0);
      CHECK(sensei_commission_prover_share(&pro, share_p) == 0);
      CHECK(memcmp(share_p, s->share_p, 65) == 0);
      CHECK(sensei_commission_verifier_pake1(&ver, 1010, share_p, share_v, confirm_v) == 0);
      CHECK(memcmp(share_v, s->share_v, 65) == 0 && memcmp(confirm_v, s->confirm_v, 32) == 0);
      CHECK(counters_are(1, 1) && F.writes == 1);
      CHECK(sensei_commission_verifier_sid(&ver) == 0);
      CHECK(sensei_commission_prover_finish(&pro, share_v, confirm_v, confirm_p) == 0);
      CHECK(memcmp(confirm_p, s->confirm_p, 32) == 0);
      CHECK(sensei_commission_verifier_pake3(&ver, 1020, confirm_p) == 0);
      CHECK(sensei_commission_verifier_state(&ver) == SENSEI_COMMISSION_OPEN);
      CHECK(sensei_commission_verifier_sid(&ver) == s->sid);
      CHECK(counters_are(0, 0) && F.writes == 2);
      /* the channel uses the keys of the vector: the frames are what a seal with those keys gives */
      {
        sensei_secure_key_t kc = 0, kn = 0;
        uint8_t msg[3] = {1, 2, 3}, ref[64], out[64];
        size_t ol = 0;

        CHECK(C0->aead_import(C0->ctx, s->k_c2n, &kc) == 0 && C0->aead_import(C0->ctx, s->k_n2c, &kn) == 0);
        n = sensei_commission_prover_seal(&pro, msg, 3, frame, sizeof(frame));
        CHECK(n == 30 + 3 && sensei_secure_seal_commission(C0, kc, s->sid, true, 1, msg, 3, ref, sizeof(ref)) == n &&
              memcmp(frame, ref, (size_t)n) == 0);
        CHECK(sensei_commission_verifier_recv(&ver, 1030, frame, (size_t)n, out, sizeof(out), &ol) == 0 && ol == 3 &&
              memcmp(out, msg, 3) == 0);
        n2 = sensei_commission_verifier_seal(&ver, 1040, msg, 3, frame2, sizeof(frame2));
        CHECK(n2 == 26 + 3 && sensei_secure_seal_commission(C0, kn, s->sid, false, 1, msg, 3, ref, sizeof(ref)) == n2 &&
              memcmp(frame2, ref, (size_t)n2) == 0);
        CHECK(sensei_commission_prover_recv(&pro, frame2, (size_t)n2, out, sizeof(out), &ol) == 0 && ol == 3);
        C0->key_destroy(C0->ctx, kc);
        C0->key_destroy(C0->ctx, kn);
      }
      sensei_commission_verifier_end(&ver);
      sensei_commission_prover_end(&pro);
      CHECK(is_zeros(&ver, sizeof(ver)) && is_zeros(&pro, sizeof(pro)));
    }
  }
}

/* ---- V12: the channel, two receivers that start empty ---- */

static void test_v12(void) {
  const vec_com_ses_t *s = &vec_com_ses[0];
  struct sensei_commission_channel node, host, rx_a, rx_b;
  struct sensei_commission_channel *chs[2] = {&host, &node}; /* role 0: the commissioner receives; 1: the node */

  CHECK(sensei_commission_channel_open(&node, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, s->sid) == 0);
  CHECK(sensei_commission_channel_open(&host, C0, SENSEI_COMMISSION_PROVER, s->k_c2n, s->k_n2c, s->sid) == 0);
  for (int i = 0; i < VEC_COM_ENV_COUNT; i++) {
    const vec_com_env_t *e = &vec_com_env[i];
    uint8_t out[400];
    size_t ol = 7;
    int rc = sensei_commission_channel_recv(chs[e->role], e->env, e->env_len, out, sizeof(out), &ol);

    CHECK(rc == e->expect);
    if (rc != e->expect) {
      printf("  V12 %d (%s): %d, want %d\n", i, e->what, rc, e->expect);
    }
    if (e->expect == 0) {
      CHECK(ol == e->plain_len && memcmp(out, e->plain, ol) == 0);
      /* the same bytes come out of a seal with this counter */
      {
        uint8_t ref[400];
        struct sensei_secure_info info;
        sensei_secure_key_t k = 0;
        int n;

        CHECK(sensei_secure_peek(e->env, e->env_len, &info) == 0 && info.kind == SENSEI_SECURE_KIND_COMMISSION);
        CHECK(C0->aead_import(C0->ctx, e->role ? s->k_c2n : s->k_n2c, &k) == 0);
        n = sensei_secure_seal_commission(C0, k, s->sid, e->role == 1, info.counter, e->plain, e->plain_len, ref,
                                          sizeof(ref));
        CHECK(n == (int)e->env_len && memcmp(ref, e->env, e->env_len) == 0);
        C0->key_destroy(C0->ctx, k);
      }
    }
  }
  /* two receivers fed the same frames, one with a refused frame in between, end in the same window */
  CHECK(sensei_commission_channel_open(&rx_a, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, s->sid) == 0);
  CHECK(sensei_commission_channel_open(&rx_b, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, s->sid) == 0);
  for (int i = 0; i < 4; i++) {
    uint8_t out[400];
    size_t ol;

    CHECK(sensei_commission_channel_recv(&rx_a, vec_com_env[i].env, vec_com_env[i].env_len, out, sizeof(out), &ol) ==
          (i == 1 ? SENSEI_SECURE_ERR_REPLAY : 0));
    if (i == 1) { /* rx_b does not see the replay: the same window afterwards */
      continue;
    }
    CHECK(sensei_commission_channel_recv(&rx_b, vec_com_env[i].env, vec_com_env[i].env_len, out, sizeof(out), &ol) ==
          0);
  }
  CHECK(rx_a.rx.highest == rx_b.rx.highest && rx_a.rx.bitmap == rx_b.rx.bitmap);
  sensei_commission_channel_close(&node);
  sensei_commission_channel_close(&host);
  sensei_commission_channel_close(&rx_a);
  sensei_commission_channel_close(&rx_b);
  CHECK(is_zeros(&node, sizeof(node)));
  /* seal: limits, closed channel, counters */
  {
    uint8_t big[400] = {0}, out[400];
    struct sensei_commission_channel ch;
    int n;

    CHECK(sensei_commission_channel_open(&ch, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, s->sid) == 0);
    CHECK(sensei_commission_channel_seal(&ch, big, 294, out, sizeof(out)) == 320); /* 26 + 294 */
    CHECK(sensei_commission_channel_seal(&ch, big, 295, out, sizeof(out)) == SENSEI_SECURE_ERR_SPACE);
    CHECK(ch.tx_counter == 1); /* a seal that failed used no counter */
    CHECK(sensei_commission_channel_seal(&ch, big, 10, out, 35) == SENSEI_SECURE_ERR_SPACE);
    n = sensei_commission_channel_seal(&ch, big, 10, out, 36);
    CHECK(n == 36 && ch.tx_counter == 2);
    ch.tx_counter = 0xFFFFFFFFu;
    CHECK(sensei_commission_channel_seal(&ch, big, 1, out, sizeof(out)) == SENSEI_SECURE_ERR_STATE);
    sensei_commission_channel_close(&ch);
    CHECK(sensei_commission_channel_seal(&ch, big, 1, out, sizeof(out)) == SENSEI_SECURE_ERR_STATE);
    CHECK(sensei_secure_seal_commission(C0, 0, s->sid, false, 0, big, 1, out, sizeof(out)) == SENSEI_SECURE_ERR_STATE);
    CHECK(sensei_commission_channel_open(&ch, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, 0) ==
          SENSEI_SECURE_ERR_STATE);
  }
}

/* ---- V13: fingerprints and the proof ---- */

static void test_v13(void) {
  uint8_t fp[16], cfp[8], plain[64];

  for (int i = 0; i < VEC_COM_FP_COUNT; i++) {
    CHECK(sensei_commission_fingerprint(C0, vec_com_fp[i].node_id, vec_com_fp[i].node_pub, fp) == 0);
    CHECK(memcmp(fp, vec_com_fp[i].fp, 16) == 0);
  }
  CHECK(sensei_commission_collector_fingerprint(C0, vec_com_cfp_pub, cfp) == 0 && memcmp(cfp, vec_com_cfp, 8) == 0);
  for (int i = 0; i < VEC_COM_POP_COUNT; i++) {
    const vec_com_pop_t *p = &vec_com_pop[i];
    struct sensei_secure_keys k;
    struct sensei_secure node;
    uint8_t env[200];
    int n;

    sensei_commission_proof_plain(p->node_id, p->nonce_g, p->nonce_n, plain);
    CHECK(p->plain_len == 64 && memcmp(plain, p->plain, 64) == 0);
    /* the proof is a kind 3 envelope under k_up with the counter of the node: the normal seal path */
    memcpy(k.k_up, vec_com_pop_k_up, 16);
    memset(k.k_down, 7, 16);
    k.key_id = VEC_COM_POP_KEY_ID;
    CHECK(sensei_secure_init(&node, C0, SENSEI_SECURE_ROLE_NODE, &k, 0, p->counter - 1, 1000, NULL, 0, NULL) == 0);
    n = sensei_secure_seal(&node, SENSEI_SECURE_KIND_NODE_TO_HOST, NULL, plain, 64, env, sizeof(env));
    CHECK(n == (int)p->proof_len && memcmp(env, p->proof, p->proof_len) == 0);
    sensei_secure_close(&node);
  }
}

/* ---- V14: setup code, QR, label ---- */

static void test_v14(void) {
  char code[13], text[80], l1[25], l2[26], l3[24];
  uint64_t id;
  uint8_t fp[16];

  for (int i = 0; i < VEC_COM_VH_COUNT; i++) {
    size_t n = strlen(vec_com_vh[i].digits);

    CHECK(sensei_commission_verhoeff_digit(vec_com_vh[i].digits, n) == vec_com_vh[i].check);
    if (n == 11) {
      char c12[13];

      memcpy(c12, vec_com_vh[i].digits, 11);
      c12[11] = (char)('0' + vec_com_vh[i].check);
      c12[12] = 0;
      CHECK(sensei_commission_code_parse(c12, code) == 0 && strcmp(code, c12) == 0);
    }
  }
  CHECK(sensei_commission_verhoeff_digit("12a", 3) == -1);
  for (int i = 0; i < VEC_COM_BAD_CODE_COUNT; i++) {
    CHECK(sensei_commission_code_parse(vec_com_bad_code[i], code) == SENSEI_SECURE_ERR_FORMAT);
  }
  CHECK(sensei_commission_code_parse(" 0000-0000 0003\n", code) == 0 && strcmp(code, "000000000003") == 0);
  CHECK(sensei_commission_code_parse("00000000003", code) < 0 &&
        sensei_commission_code_parse("0000000000030", code) < 0 &&
        sensei_commission_code_parse("00000000000a", code) < 0 && sensei_commission_code_parse(NULL, code) < 0 &&
        sensei_commission_code_parse("", code) < 0);
  for (int i = 0; i < VEC_COM_QR_COUNT; i++) {
    const vec_com_qr_t *q = &vec_com_qr[i];

    CHECK(sensei_commission_qr_format(q->node_id, q->code, q->fp, text) == 0 && strcmp(text, q->qr) == 0);
    CHECK(strlen(text) == SENSEI_COMMISSION_QR_SIZE);
    memset(code, 0, sizeof(code));
    CHECK(sensei_commission_qr_parse(q->qr, &id, code, fp) == 0 && id == q->node_id && strcmp(code, q->code) == 0 &&
          memcmp(fp, q->fp, 16) == 0);
    {
      char padded[100];

      snprintf(padded, sizeof(padded), "  %s\n", q->qr);
      CHECK(sensei_commission_qr_parse(padded, &id, code, fp) == 0);
    }
    sensei_commission_code_format(q->code, text);
    CHECK(strcmp(text, q->code_text) == 0);
    sensei_commission_label_lines(q->node_id, q->code, q->fp, l1, l2, l3);
    CHECK(strcmp(l1, q->line_id) == 0 && strcmp(l2, q->line_code) == 0 && strcmp(l3, q->line_key) == 0);
  }
  for (int i = 0; i < VEC_COM_BADQR_COUNT; i++) {
    id = 99;
    CHECK(sensei_commission_qr_parse(vec_com_badqr[i].text, &id, code, fp) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(id == 99);
  }
  CHECK(sensei_commission_qr_parse("SN1:0000000000000000:000000000003:0D3EE8C72CBA1AAEE17FA018D112F75F", &id, code,
                                   fp) < 0); /* node id 0 */
  CHECK(sensei_commission_qr_format(0, "000000000003", fp, text) < 0);
  CHECK(sensei_commission_qr_format(1, "000000000004", fp, text) < 0);
  CHECK(sensei_commission_qr_format(1, "0000-0000-0003", fp, text) < 0);
  CHECK(sensei_commission_qr_parse(NULL, &id, code, fp) < 0);
}

/* ---- V15: refusals of the node ---- */

static void test_v15a(void) {
  /* kind 5 under the static keys at the normal receivers: refused, by every open path, with no counter used */
  struct sensei_secure_keys kn, kb;
  sensei_secure_key_t pn = 0, pb = 0;
  struct sensei_secure sess[2];
  static const enum sensei_secure_kind wants[] = {0,
                                                  SENSEI_SECURE_KIND_UPLINK,
                                                  SENSEI_SECURE_KIND_DOWNLINK,
                                                  SENSEI_SECURE_KIND_NODE_TO_HOST,
                                                  SENSEI_SECURE_KIND_SETTINGS,
                                                  SENSEI_SECURE_KIND_COMMISSION};

  CHECK(C0->ecdh_import(C0->ctx, vec_sec_node_priv, 32, &pn) == 0 &&
        C0->ecdh_import(C0->ctx, vec_sec_backend_priv, 32, &pb) == 0);
  CHECK(sensei_secure_derive(C0, pn, SENSEI_SECURE_ROLE_NODE, vec_sec_node_pub, vec_sec_backend_pub, &kn) == 0);
  CHECK(sensei_secure_derive(C0, pb, SENSEI_SECURE_ROLE_BACKEND, vec_sec_node_pub, vec_sec_backend_pub, &kb) == 0);
  C0->key_destroy(C0->ctx, pn);
  C0->key_destroy(C0->ctx, pb);
  CHECK(sensei_secure_init(&sess[0], C0, SENSEI_SECURE_ROLE_BACKEND, &kb, 0x01020304u, 0, 100, NULL, 0, NULL) == 0);
  CHECK(sensei_secure_init(&sess[1], C0, SENSEI_SECURE_ROLE_NODE, &kn, 0, 0, 100, NULL, 0, NULL) == 0);
  for (int i = 0; i < VEC_COM_X5_COUNT; i++) {
    const vec_com_x5_t *x = &vec_com_x5[i];
    struct sensei_secure_info info;

    CHECK(x->env_len >= 26);
    CHECK(sensei_secure_peek(x->env, x->env_len, &info) == 0 && info.kind == 5 && info.key_id == VEC_SEC_KEY_ID);
    for (size_t w = 0; w < sizeof(wants) / sizeof(wants[0]); w++) {
      uint8_t out[400] = {0};
      size_t ol = 0;
      enum sensei_secure_kind kind = 0;
      int rc = sensei_secure_open(&sess[x->receiver], wants[w], NULL, x->env, x->env_len, out, sizeof(out), &ol, &kind);

      CHECK(rc == x->expect);
      CHECK(is_zeros(out, sizeof(out)) && ol == 0);
    }
    CHECK(sensei_secure_rx_highest(&sess[x->receiver], x->receiver ? 1u : 0u) == 0);
    CHECK(sess[x->receiver].n_peers == 0); /* no sender slot taken, no counter consumed */
  }
  /* the seal side refuses kind 5 too */
  {
    uint8_t out[100];

    CHECK(sensei_secure_seal(&sess[1], SENSEI_SECURE_KIND_COMMISSION, NULL, out, 4, out, sizeof(out)) ==
          SENSEI_SECURE_ERR_KIND);
    CHECK(sensei_secure_seal(&sess[0], SENSEI_SECURE_KIND_COMMISSION, NULL, out, 4, out, sizeof(out)) ==
          SENSEI_SECURE_ERR_KIND);
  }
  sensei_secure_close(&sess[0]);
  sensei_secure_close(&sess[1]);
}

/* A kind 5 frame of the real channel is refused the same way by the normal receivers whatever the key. */
static void test_x5_channel(void) {
  const vec_com_ses_t *s = &vec_com_ses[0];
  struct sensei_secure_keys k;
  struct sensei_secure sess;
  uint8_t out[100];
  size_t ol = 0;

  memcpy(k.k_up, s->k_n2c, 16);
  memcpy(k.k_down, s->k_c2n, 16);
  k.key_id = s->sid;
  CHECK(sensei_secure_init(&sess, C0, SENSEI_SECURE_ROLE_NODE, &k, 0, 0, 100, NULL, 0, NULL) == 0);
  /* vec_com_env_0 is a kind 5 frame to the node under k_c2n with this session id: the node session would open it if
   * the kind were not refused */
  CHECK(sensei_secure_open(&sess, 0, NULL, vec_com_env[0].env, vec_com_env[0].env_len, out, sizeof(out), &ol, NULL) ==
        SENSEI_SECURE_ERR_FORMAT);
  CHECK(sensei_secure_open(&sess, SENSEI_SECURE_KIND_COMMISSION, NULL, vec_com_env[0].env, vec_com_env[0].env_len, out,
                           sizeof(out), &ol, NULL) == SENSEI_SECURE_ERR_FORMAT);
  CHECK(sess.n_peers == 0);
  sensei_secure_close(&sess);
  /* headers of kind 5 that the parser must refuse */
  {
    uint8_t f[40];
    struct sensei_secure_info info;

    memcpy(f, vec_com_env[0].env, 30);
    CHECK(sensei_secure_peek(f, 30, &info) == 0 && info.to_node && info.sender == 1 && info.kind == 5 &&
          !info.has_node_id && info.key_id == s->sid);
    f[0] |= 2; /* node id flag */
    CHECK(sensei_secure_peek(f, 30, &info) < 0);
    f[0] &= (uint8_t)~2u;
    f[1] |= 0x80; /* ratchet bit */
    CHECK(sensei_secure_peek(f, 30, &info) < 0);
    f[1] = 0x45; /* reserved bit */
    CHECK(sensei_secure_peek(f, 30, &info) < 0);
    f[1] = 0x35; /* generation bits without the ratchet bit */
    CHECK(sensei_secure_peek(f, 30, &info) < 0);
    f[1] = 5;
    f[6] = 2; /* sender 2 */
    CHECK(sensei_secure_peek(f, 30, &info) < 0);
    f[6] = 1;
    CHECK(sensei_secure_peek(f, 30, &info) == 0);
    f[0] &= (uint8_t)~1u; /* direction 0 has no sender field: the header is shorter and still fine */
    CHECK(sensei_secure_peek(f, 30, &info) == 0 && !info.to_node && info.sender == 0);
  }
}

static void test_v15b(void) {
  for (int i = 0; i < VEC_COM_W0M_COUNT; i++) {
    const vec_com_w0m_t *w = &vec_com_w0m[i];
    const vec_com_ses_t *s = &vec_com_ses[w->ses];
    struct sensei_commission_record rec;
    struct sensei_commission_verifier v;
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nonce_n[16], sv[65], cv[32];

    record_of(s->reg, &rec);
    flash_reset(3, 2);
    ct_init();
    queue_verifier(s);
    cfg = cfg_for(&rec, s->node_id);
    CHECK(sensei_commission_verifier_begin(&v, &cfg, s->nonce_g, 0, nonce_n) == 0);
    memset(sv, 0xEE, sizeof(sv));
    memset(cv, 0xEE, sizeof(cv));
    CHECK(sensei_commission_verifier_pake1(&v, 10, w->share_p, sv, cv) == w->expect);
    CHECK(counters_are(3, 2) && F.writes == 0 && w->counts_attempt == 0);
    CHECK(sv[0] == 0xEE && cv[0] == 0xEE); /* nothing is given out */
    CHECK(is_zeros(&v, sizeof(v)));
    CHECK(sensei_commission_verifier_state(&v) == SENSEI_COMMISSION_IDLE);
  }
}

static void test_v15c(void) {
  struct sensei_commission_record rec;
  uint8_t out[121];

  for (int i = 0; i < VEC_COM_BADREC_COUNT; i++) {
    const vec_com_badrec_t *b = &vec_com_badrec[i];

    memset(&rec, 0x5A, sizeof(rec));
    CHECK(sensei_commission_record_decode(C0, b->data, b->len, &rec) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(((uint8_t *)&rec)[0] == 0x5A && ((uint8_t *)&rec)[sizeof(rec) - 1] == 0x5A); /* nothing written */
    if (b->len == 121) { /* the encoder applies the same rules to the fields */
      struct sensei_commission_record f;

      memset(&f, 0, sizeof(f));
      f.params.suite = b->data[1];
      f.params.kdf = b->data[2];
      f.params.log2n = b->data[3];
      f.params.r = b->data[4];
      f.params.p = b->data[5];
      memcpy(f.params.salt, b->data + 8, 16);
      memcpy(f.w0, b->data + 24, 32);
      memcpy(f.l, b->data + 56, 65);
      if (b->data[0] == 1 && b->data[6] == 0 && b->data[7] == 0) { /* the fields are all it can see */
        CHECK(sensei_commission_record_encode(C0, &f, out) == SENSEI_SECURE_ERR_FORMAT);
      }
    }
  }
  CHECK(sensei_commission_record_decode(C0, NULL, 121, &rec) < 0);
  CHECK(sensei_commission_record_decode(C0, vec_com_reg[0].record, 121, &rec) == 0);
}

static void test_v15d(void) {
  const vec_com_ses_t *s = &vec_com_ses[VEC_COM_WRONG_SES];
  struct sensei_commission_record rec;
  struct sensei_commission_verifier v;
  struct sensei_commission_verifier_cfg cfg;
  uint8_t nonce_n[16], sv[65], cv[32];

  record_of(s->reg, &rec);
  flash_reset(0, 0);
  ct_init();
  queue_verifier(s);
  cfg = cfg_for(&rec, s->node_id);
  CHECK(sensei_commission_verifier_begin(&v, &cfg, s->nonce_g, 0, nonce_n) == 0);
  CHECK(sensei_commission_verifier_pake1(&v, 5, vec_com_wrong_share_p, sv, cv) == 0);
  CHECK(memcmp(sv, vec_com_wrong_share_v, 65) == 0 && memcmp(cv, vec_com_wrong_confirm_v, 32) == 0);
  CHECK(counters_are(1, 1) && F.writes == 1); /* the attempt counted before the answer */
  CHECK(sensei_commission_verifier_pake3(&v, 6, vec_com_wrong_confirm_p) == VEC_COM_WRONG_EXPECT);
  CHECK(counters_are(1, 1) && F.writes == 1); /* and stays counted */
  CHECK(is_zeros(&v, sizeof(v)));
  CHECK(sensei_commission_verifier_pake3(&v, 7, vec_com_wrong_confirm_p) == SENSEI_SECURE_ERR_STATE);
  (void)vec_com_wrong_code;
}

/* ---- sessions: abort at every step, tamper, out of order, time, counters ---- */

struct pair {
  struct sensei_commission_verifier v;
  struct sensei_commission_prover p;
  struct sensei_commission_record rec;
  uint8_t w0[32], w1[32];
  uint8_t nonce_g[16], nonce_n[16];
  uint8_t share_p[65], share_v[65], confirm_v[32], confirm_p[32];
};

static const uint64_t NODE = 0x00A1B2C3D4E5F607ull;

/* Verifier begun with a fixed y and nonces; prover begun with the registration 0 secrets. */
static void pair_begin(struct pair *a, uint32_t now) {
  const vec_com_reg_t *r = &vec_com_reg[0];
  struct sensei_commission_verifier_cfg cfg;

  record_of(0, &a->rec);
  memcpy(a->w0, r->w0, 32);
  memcpy(a->w1, r->w1, 32);
  memset(a->nonce_g, 0x31, 16);
  ct_init();
  rng_put(vec_com_ses[0].nonce_n, 16);
  rng_put_scalar(vec_com_ses[0].x);
  rng_put_scalar(vec_com_ses[0].y);
  cfg = cfg_for(&a->rec, NODE);
  CHECK(sensei_commission_verifier_begin(&a->v, &cfg, a->nonce_g, now, a->nonce_n) == 0);
  CHECK(sensei_commission_prover_begin(&a->p, &CT, a->w0, a->w1, NODE, &a->rec.params, a->nonce_g, a->nonce_n) == 0);
  CHECK(sensei_commission_prover_share(&a->p, a->share_p) == 0);
}

static void pair_pake1(struct pair *a, uint32_t now) {
  CHECK(sensei_commission_verifier_pake1(&a->v, now, a->share_p, a->share_v, a->confirm_v) == 0);
  CHECK(sensei_commission_prover_finish(&a->p, a->share_v, a->confirm_v, a->confirm_p) == 0);
}

static void test_flow(void) {
  struct pair a;

  /* the whole run with real random numbers */
  {
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nn[16];
    const vec_com_reg_t *r = &vec_com_reg[1];
    struct sensei_commission_record rec;
    uint8_t out[300];
    size_t ol;
    int n;

    record_of(1, &rec);
    flash_reset(0, 0);
    ct_init();
    CT.random = C0->random;
    CT.ctx = C0->ctx;
    cfg = cfg_for(&rec, NODE);
    memset(a.nonce_g, 9, 16);
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == 0);
    CHECK(sensei_commission_prover_begin(&a.p, &CT, r->w0, r->w1, NODE, &rec.params, a.nonce_g, nn) == 0);
    CHECK(sensei_commission_prover_share(&a.p, a.share_p) == 0);
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, a.share_v, a.confirm_v) == 0);
    CHECK(sensei_commission_prover_finish(&a.p, a.share_v, a.confirm_v, a.confirm_p) == 0);
    CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
    for (int i = 0; i < 5; i++) { /* frames both ways, counters go up */
      uint8_t msg[8] = {(uint8_t)i};

      n = sensei_commission_prover_seal(&a.p, msg, sizeof(msg), out, sizeof(out));
      CHECK(n > 0 &&
            sensei_commission_verifier_recv(&a.v, 3 + (uint32_t)i, out, (size_t)n, msg, sizeof(msg), &ol) == 0);
      n = sensei_commission_verifier_seal(&a.v, 3 + (uint32_t)i, msg, sizeof(msg), out, sizeof(out));
      CHECK(n > 0 && sensei_commission_prover_recv(&a.p, out, (size_t)n, msg, sizeof(msg), &ol) == 0);
    }
    CHECK(a.v.ch.tx_counter == 5 && a.p.ch.tx_counter == 5);
    sensei_commission_verifier_end(&a.v);
    sensei_commission_prover_end(&a.p);
  }
  /* one random draw error at each point */
  for (int k = 1; k <= 3; k++) {
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nn[16], sv[65], cv[32], cp[32];

    flash_reset(0, 0);
    record_of(0, &a.rec);
    ct_init();
    rng_put(vec_com_ses[0].nonce_n, 16);
    rng_put_scalar(vec_com_ses[0].x);
    rng_put_scalar(vec_com_ses[0].y);
    R.fail_random = k;
    cfg = cfg_for(&a.rec, NODE);
    memset(a.nonce_g, 1, 16);
    if (sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) != 0) { /* k = 1: nonce_n */
      CHECK(k == 1 && is_zeros(&a.v, sizeof(a.v)));
      continue;
    }
    if (sensei_commission_prover_begin(&a.p, &CT, vec_com_reg[0].w0, vec_com_reg[0].w1, NODE, &a.rec.params, a.nonce_g,
                                       nn) != 0) { /* k = 2: x */
      CHECK(k == 2 && is_zeros(&a.p, sizeof(a.p)));
      sensei_commission_verifier_end(&a.v);
      continue;
    }
    CHECK(sensei_commission_prover_share(&a.p, a.share_p) == 0);
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, sv, cv) < 0); /* k = 3: y */
    CHECK(is_zeros(&a.v, sizeof(a.v)) && counters_are(0, 0) && F.writes == 0);
    (void)cp;
    sensei_commission_prover_end(&a.p);
  }
}

static void test_aborts(void) {
  struct pair a;
  uint8_t out[100], sv[65], cv[32];
  size_t ol;

  /* begin errors */
  {
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nn[16];

    record_of(0, &a.rec);
    memset(a.nonce_g, 1, 16);
    flash_reset(0, 0);
    ct_init();
    rng_put(vec_com_ses[0].nonce_n, 16);
    cfg = cfg_for(&a.rec, 0); /* node id 0 means any: not a node */
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(is_zeros(&a.v, sizeof(a.v)));
    cfg = cfg_for(&a.rec, NODE);
    CHECK(sensei_commission_verifier_begin(&a.v, NULL, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_STATE);
    cfg.save_counters = NULL;
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_STATE);
    cfg = cfg_for(&a.rec, NODE);
    cfg.counters = NULL;
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_STATE);
    cfg = cfg_for(&a.rec, NODE);
    cfg.c = &CT;
    CT.sha256 = NULL; /* a backend without commissioning */
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_STATE);
    CT.sha256 = C0->sha256;
    memset(a.rec.w0, 0, 32); /* w0 = 0 */
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_FORMAT);
    record_of(0, &a.rec);
    a.rec.l[64] ^= 1; /* off the curve */
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_FORMAT);
    record_of(0, &a.rec);
    a.rec.params.log2n = 13;
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(is_zeros(&a.v, sizeof(a.v)));
  }
  /* out of order calls end the session and wipe it */
  pair_begin(&a, 0);
  CHECK(sensei_commission_verifier_pake3(&a.v, 1, a.confirm_p) == SENSEI_SECURE_ERR_STATE); /* pake3 before pake1 */
  CHECK(is_zeros(&a.v, sizeof(a.v)));
  CHECK(sensei_commission_verifier_pake1(&a.v, 2, a.share_p, sv, cv) == SENSEI_SECURE_ERR_STATE); /* ended */
  sensei_commission_prover_end(&a.p);

  pair_begin(&a, 0);
  CHECK(sensei_commission_verifier_seal(&a.v, 1, (const uint8_t *)"x", 1, out, sizeof(out)) == SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_commission_verifier_recv(&a.v, 1, out, 40, out, sizeof(out), &ol) == SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_commission_verifier_state(&a.v) == SENSEI_COMMISSION_BEGUN); /* a stray frame does not end it */
  pair_pake1(&a, 2);
  CHECK(sensei_commission_verifier_pake1(&a.v, 3, a.share_p, sv, cv) == SENSEI_SECURE_ERR_STATE); /* pake1 twice */
  CHECK(is_zeros(&a.v, sizeof(a.v)) && counters_are(1, 1)); /* the second one counted nothing */
  CHECK(sensei_commission_prover_finish(&a.p, a.share_v, a.confirm_v, a.confirm_p) == SENSEI_SECURE_ERR_STATE);
  CHECK(sensei_commission_prover_share(&a.p, a.share_p) == SENSEI_SECURE_ERR_STATE);
  sensei_commission_prover_end(&a.p);
  CHECK(is_zeros(&a.p, sizeof(a.p)));
  CHECK(sensei_commission_prover_seal(&a.p, out, 1, out, sizeof(out)) == SENSEI_SECURE_ERR_STATE); /* ended */

  /* pake3 twice */
  flash_reset(0, 0);
  pair_begin(&a, 0);
  pair_pake1(&a, 1);
  CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
  CHECK(sensei_commission_verifier_pake3(&a.v, 3, a.confirm_p) == SENSEI_SECURE_ERR_STATE);
  CHECK(is_zeros(&a.v, sizeof(a.v)));
  CHECK(sensei_commission_prover_finish(&a.p, a.share_v, a.confirm_v, a.confirm_p) == SENSEI_SECURE_ERR_STATE);
  sensei_commission_prover_end(&a.p);
  sensei_commission_prover_end(&a.p); /* twice is fine */

  /* abort at each step: end() wipes whatever there is, the counters are as they were */
  for (int step = 0; step < 4; step++) {
    flash_reset(4, 3);
    pair_begin(&a, 0);
    if (step >= 1) {
      pair_pake1(&a, 1);
    }
    if (step >= 2) {
      CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
    }
    if (step >= 3) {
      uint8_t msg[4] = {1, 2, 3, 4};
      int n = sensei_commission_prover_seal(&a.p, msg, 4, out, sizeof(out));

      CHECK(sensei_commission_verifier_recv(&a.v, 3, out, (size_t)n, msg, 4, &ol) == 0);
    }
    sensei_commission_verifier_end(&a.v);
    sensei_commission_prover_end(&a.p);
    CHECK(is_zeros(&a.v, sizeof(a.v)) && is_zeros(&a.p, sizeof(a.p)));
    CHECK(step == 0 ? counters_are(4, 3) : (step == 1 ? counters_are(5, 4) : counters_are(4, 0)));
  }
}

static void test_tamper(void) {
  struct pair a;
  uint8_t sv[65], cv[32], bad[65];

  /* every byte of shareP changed: never accepted as a valid exchange; an invalid point is not counted, a valid
   * one changed by chance does not exist here (a single changed byte is off the curve) */
  for (size_t i = 0; i < 65; i += 4) {
    flash_reset(0, 0);
    pair_begin(&a, 0);
    memcpy(bad, a.share_p, 65);
    bad[i] ^= 0x01;
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, bad, sv, cv) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(counters_are(0, 0) && F.writes == 0 && is_zeros(&a.v, sizeof(a.v)));
    sensei_commission_prover_end(&a.p);
  }
  /* a valid share of another exchange (another x): accepted by the node (valid point, counted), the confirmation fails
   * on both sides */
  {
    static const uint8_t x2[32] = {2};
    uint8_t other[65];

    flash_reset(0, 0);
    pair_begin(&a, 0);
    CHECK(C0->spake2p_share(C0->ctx, a.w0, x2, sensei_commission_m, other) == 0);
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, other, a.share_v, a.confirm_v) == 0);
    CHECK(counters_are(1, 1));
    /* the prover answers with its own confirmation: the node's check of it fails */
    CHECK(sensei_commission_prover_finish(&a.p, a.share_v, a.confirm_v, a.confirm_p) == SENSEI_SECURE_ERR_AUTH);
    CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == SENSEI_SECURE_ERR_AUTH);
    CHECK(counters_are(1, 1) && is_zeros(&a.v, sizeof(a.v)) && is_zeros(&a.p, sizeof(a.p)));
  }
  /* confirmP: every bit of it matters */
  for (int bit = 0; bit < 256; bit += 17) {
    uint8_t c[32];

    flash_reset(0, 0);
    pair_begin(&a, 0);
    pair_pake1(&a, 1);
    memcpy(c, a.confirm_p, 32);
    c[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    CHECK(sensei_commission_verifier_pake3(&a.v, 2, c) == SENSEI_SECURE_ERR_AUTH);
    CHECK(counters_are(1, 1) && is_zeros(&a.v, sizeof(a.v)));
    sensei_commission_prover_end(&a.p);
  }
  /* shareV and confirmV changed on the way to the prover */
  for (int what = 0; what < 3; what++) {
    uint8_t sv2[65], cv2[32], cp[32];

    flash_reset(0, 0);
    pair_begin(&a, 0);
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, sv, cv) == 0);
    memcpy(sv2, sv, 65);
    memcpy(cv2, cv, 32);
    if (what == 0) {
      sv2[64] ^= 1; /* off the curve */
    } else if (what == 1) {
      cv2[0] ^= 1;
    } else {
      memset(sv2, 0, 65); /* the identity encoding */
    }
    CHECK(sensei_commission_prover_finish(&a.p, sv2, cv2, cp) ==
          (what == 1 ? SENSEI_SECURE_ERR_AUTH : SENSEI_SECURE_ERR_FORMAT));
    CHECK(is_zeros(&a.p, sizeof(a.p)));
    sensei_commission_verifier_end(&a.v);
  }
  /* a shareV that is w0*N at the prover (rest = identity) */
  {
    static const uint8_t zero[32];
    uint8_t w0n[65], cp[32];

    flash_reset(0, 0);
    pair_begin(&a, 0);
    CHECK(C0->spake2p_share(C0->ctx, a.w0, zero, sensei_commission_n, w0n) == 0); /* 0*G + w0*N */
    CHECK(sensei_commission_prover_finish(&a.p, w0n, a.confirm_v, cp) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(is_zeros(&a.p, sizeof(a.p)));
    sensei_commission_verifier_end(&a.v);
  }
  /* kind 5 frames in the secure phase: junk does not end the session */
  {
    uint8_t out[100], msg[4] = {9, 8, 7, 6};
    size_t ol;
    int n;

    flash_reset(0, 0);
    pair_begin(&a, 0);
    pair_pake1(&a, 1);
    CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
    n = sensei_commission_prover_seal(&a.p, msg, 4, out, sizeof(out));
    CHECK(n > 0);
    out[n - 1] ^= 1; /* tag changed */
    CHECK(sensei_commission_verifier_recv(&a.v, 3, out, (size_t)n, msg, sizeof(msg), &ol) == SENSEI_SECURE_ERR_AUTH);
    out[n - 1] ^= 1;
    CHECK(sensei_commission_verifier_recv(&a.v, 4, out, 5, msg, sizeof(msg), &ol) == SENSEI_SECURE_ERR_FORMAT);
    CHECK(sensei_commission_verifier_recv(&a.v, 5, out, (size_t)n, msg, 2, &ol) == SENSEI_SECURE_ERR_SPACE);
    CHECK(sensei_commission_verifier_recv(&a.v, 6, out, (size_t)n, msg, sizeof(msg), &ol) == 0); /* the frame itself */
    CHECK(sensei_commission_verifier_recv(&a.v, 7, out, (size_t)n, msg, sizeof(msg), &ol) == SENSEI_SECURE_ERR_REPLAY);
    CHECK(sensei_commission_verifier_state(&a.v) == SENSEI_COMMISSION_OPEN);
    sensei_commission_verifier_end(&a.v);
    sensei_commission_prover_end(&a.p);
  }
}

static void test_time(void) {
  struct pair a;
  uint8_t out[100], msg[2] = {1, 2};
  size_t ol;
  int n;

  /* begin to pake1: 30 s (the limit itself is too late) */
  flash_reset(0, 0);
  pair_begin(&a, 1000);
  CHECK(sensei_commission_verifier_poll(&a.v, 1000 + 29999) == 0);
  CHECK(sensei_commission_verifier_pake1(&a.v, 1000 + 30000, a.share_p, a.share_v, a.confirm_v) ==
        SENSEI_COMMISSION_ERR_TIMEOUT);
  CHECK(is_zeros(&a.v, sizeof(a.v)) && counters_are(0, 0));
  sensei_commission_prover_end(&a.p);
  pair_begin(&a, 1000);
  CHECK(sensei_commission_verifier_pake1(&a.v, 1000 + 29999, a.share_p, a.share_v, a.confirm_v) == 0);
  /* pake2 to pake3: 15 s */
  CHECK(sensei_commission_verifier_poll(&a.v, 1000 + 29999 + 14999) == 0);
  CHECK(sensei_commission_verifier_poll(&a.v, 1000 + 29999 + 15000) == SENSEI_COMMISSION_ERR_TIMEOUT);
  CHECK(is_zeros(&a.v, sizeof(a.v)) && counters_are(1, 1)); /* the attempt stays counted */
  CHECK(sensei_commission_verifier_poll(&a.v, 0) == 0);     /* idle: nothing to time */
  sensei_commission_prover_end(&a.p);

  pair_begin(&a, 0);
  pair_pake1(&a, 100);
  CHECK(sensei_commission_verifier_pake3(&a.v, 100 + 15000, a.confirm_p) == SENSEI_COMMISSION_ERR_TIMEOUT);
  CHECK(is_zeros(&a.v, sizeof(a.v)));
  sensei_commission_prover_end(&a.p);

  /* secure phase: idle 60 s, reset by a frame in either direction; 180 s in all */
  flash_reset(0, 0);
  pair_begin(&a, 0);
  pair_pake1(&a, 100);
  CHECK(sensei_commission_verifier_pake3(&a.v, 200, a.confirm_p) == 0);
  n = sensei_commission_verifier_seal(&a.v, 200 + 59999, msg, 2, out,
                                      sizeof(out)); /* just in time, resets the idle time */
  CHECK(n > 0);
  CHECK(sensei_commission_verifier_poll(&a.v, 200 + 59999 + 59999) == 0);
  n = sensei_commission_prover_seal(&a.p, msg, 2, out, sizeof(out));
  CHECK(sensei_commission_verifier_recv(&a.v, 200 + 59999 + 59999, out, (size_t)n, msg, 2, &ol) == 0);
  for (uint32_t t = 200 + 2 * 59999; t < 180000; t += 50000) { /* traffic every 50 s */
    n = sensei_commission_prover_seal(&a.p, msg, 2, out, sizeof(out));
    CHECK(sensei_commission_verifier_recv(&a.v, t, out, (size_t)n, msg, 2, &ol) == 0);
  }
  n = sensei_commission_prover_seal(&a.p, msg, 2, out, sizeof(out));
  CHECK(sensei_commission_verifier_recv(&a.v, 180000, out, (size_t)n, msg, 2, &ol) == SENSEI_COMMISSION_ERR_TIMEOUT);
  CHECK(is_zeros(&a.v, sizeof(a.v)));
  sensei_commission_prover_end(&a.p);

  pair_begin(&a, 0);
  pair_pake1(&a, 100);
  CHECK(sensei_commission_verifier_pake3(&a.v, 200, a.confirm_p) == 0);
  CHECK(sensei_commission_verifier_seal(&a.v, 200 + 60000, msg, 2, out, sizeof(out)) == SENSEI_COMMISSION_ERR_TIMEOUT);
  CHECK(is_zeros(&a.v, sizeof(a.v)));
  sensei_commission_prover_end(&a.p);

  /* limits of the config, and a clock that wraps around */
  {
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nn[16];

    record_of(0, &a.rec);
    memset(a.nonce_g, 0x31, 16);
    flash_reset(0, 0);
    ct_init();
    rng_put(vec_com_ses[0].nonce_n, 16);
    rng_put_scalar(vec_com_ses[0].y);
    cfg = cfg_for(&a.rec, NODE);
    cfg.limits.begin_ms = 5000;
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0xFFFFFFF0u, nn) == 0);
    CHECK(sensei_commission_verifier_poll(&a.v, 0xFFFFFFF0u + 4999u) == 0); /* wraps past 0 */
    CHECK(sensei_commission_verifier_poll(&a.v, 0xFFFFFFF0u + 5000u) == SENSEI_COMMISSION_ERR_TIMEOUT);
  }
}

static void test_counting(void) {
  struct pair a;
  uint8_t sv[65], cv[32];

  /* lockout: 30 failures since the last success (the default) stop a session at begin and at pake1 */
  {
    struct sensei_commission_verifier_cfg cfg;
    uint8_t nn[16];

    record_of(0, &a.rec);
    memset(a.nonce_g, 1, 16);
    flash_reset(100, 30);
    ct_init();
    rng_put(vec_com_ses[0].nonce_n, 16);
    cfg = cfg_for(&a.rec, NODE);
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_COMMISSION_ERR_LOCKED);
    CHECK(is_zeros(&a.v, sizeof(a.v)) && R.calls == 0); /* nothing was drawn */
    flash_reset(100, 29);
    cfg = cfg_for(&a.rec, NODE);
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == 0);
    CNT.fails_since_ok = 30; /* locked while the session runs */
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, vec_com_ses[0].share_p, sv, cv) == SENSEI_COMMISSION_ERR_LOCKED);
    CHECK(F.writes == 0 && is_zeros(&a.v, sizeof(a.v)));
    /* a lower limit in the config */
    flash_reset(7, 5);
    cfg = cfg_for(&a.rec, NODE);
    cfg.limits.fails_max = 5;
    rng_reset();
    rng_put(vec_com_ses[0].nonce_n, 16);
    CHECK(sensei_commission_verifier_begin(&a.v, &cfg, a.nonce_g, 0, nn) == SENSEI_COMMISSION_ERR_LOCKED);
  }
  /* a failing write at pake1 takes the attempt away from the session: nothing counted, nothing given out */
  flash_reset(2, 1);
  pair_begin(&a, 0);
  F.fail = 1;
  memset(sv, 0xEE, sizeof(sv));
  memset(cv, 0xEE, sizeof(cv));
  CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, sv, cv) == SENSEI_SECURE_ERR_STORE);
  CHECK(counters_are(2, 1) && F.writes == 0 && sv[0] == 0xEE && cv[0] == 0xEE && is_zeros(&a.v, sizeof(a.v)));
  sensei_commission_prover_end(&a.p);
  /* a store that always fails: every attempt ends before shareV */
  flash_reset(0, 0);
  F.fail_always = 1;
  for (int i = 0; i < 3; i++) {
    pair_begin(&a, 0);
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, sv, cv) == SENSEI_SECURE_ERR_STORE);
    CHECK(counters_are(0, 0));
    sensei_commission_prover_end(&a.p);
  }
  /* the write comes before the answer: the hook sees the new values while the outputs are still untouched */
  /* (flash_save is called with the incremented counters, the RAM copy changes after it returned 0) */
  flash_reset(5, 4);
  pair_begin(&a, 0);
  CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, a.share_v, a.confirm_v) == 0);
  CHECK(F.stored.fails_total == 6 && F.stored.fails_since_ok == 5 && CNT.fails_total == 6);
  /* power loss here: the flash holds the attempt, a new session starts from it */
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
  CNT = F.stored; /* reload after the reset */
  CHECK(counters_are(6, 5));
  /* success: since_ok = 0, total - 1 */
  pair_begin(&a, 0);
  pair_pake1(&a, 1);
  CHECK(counters_are(7, 6));
  CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
  CHECK(counters_are(6, 0));
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
  /* success with a failing write at pake3: the session opens, the counters stay counted */
  flash_reset(3, 2);
  pair_begin(&a, 0);
  pair_pake1(&a, 1);
  F.fail = 1;
  CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
  CHECK(counters_are(4, 3) && sensei_commission_verifier_state(&a.v) == SENSEI_COMMISSION_OPEN);
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
  /* total never goes below 0, and saturates at the top */
  flash_reset(0, 0);
  pair_begin(&a, 0);
  pair_pake1(&a, 1);
  CNT.fails_total = 0;
  F.stored.fails_total = 0;
  CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0 && counters_are(0, 0));
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
  flash_reset(0xFFFFFFFFu, 3);
  pair_begin(&a, 0);
  CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, a.share_v, a.confirm_v) == 0);
  CHECK(counters_are(0xFFFFFFFFu, 4));
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
}

static void test_backend_faults(void) {
  struct pair a;
  uint8_t sv[65], cv[32];

  for (int what = 0; what < 3; what++) {
    flash_reset(0, 0);
    pair_begin(&a, 0);
    if (what == 0) {
      R.fail_share = 1;
    } else if (what == 1) {
      R.fail_hkdf = 1;
    } else {
      R.fail_zv = 1;
    }
    CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, sv, cv) < 0);
    CHECK(is_zeros(&a.v, sizeof(a.v)) && counters_are(0, 0) && F.writes == 0);
    sensei_commission_prover_end(&a.p);
  }
  /* wipe after success: nothing secret is left in the verifier between the steps */
  flash_reset(0, 0);
  pair_begin(&a, 0);
  CHECK(sensei_commission_verifier_pake1(&a.v, 1, a.share_p, a.share_v, a.confirm_v) == 0);
  CHECK(is_zeros(a.v.rec.w0, 32) && is_zeros(a.v.rec.l, 65));
  CHECK(sensei_commission_prover_finish(&a.p, a.share_v, a.confirm_v, a.confirm_p) == 0);
  CHECK(is_zeros(a.p.w0, 32) && is_zeros(a.p.w1, 32) && is_zeros(a.p.x, 32));
  CHECK(sensei_commission_verifier_pake3(&a.v, 2, a.confirm_p) == 0);
  CHECK(is_zeros(a.v.k_c2n, 16) && is_zeros(a.v.k_n2c, 16) && is_zeros(a.v.confirm_p, 32));
  sensei_commission_verifier_end(&a.v);
  sensei_commission_prover_end(&a.p);
}

/* Random garbage at the receivers of kind 5 and at the share and record parsers never opens or accepts anything. */
static void test_garbage(void) {
  const vec_com_ses_t *s = &vec_com_ses[0];
  struct sensei_commission_channel ch;
  uint8_t f[330], out[400];
  uint32_t seed = 12345;
  size_t ol;

  CHECK(sensei_commission_channel_open(&ch, C0, SENSEI_COMMISSION_VERIFIER, s->k_c2n, s->k_n2c, s->sid) == 0);
  for (int i = 0; i < 4000; i++) {
    size_t n = (size_t)(i % 330);

    for (size_t j = 0; j < sizeof(f); j++) {
      seed = seed * 1664525u + 1013904223u;
      f[j] = (uint8_t)(seed >> 24);
    }
    if (i % 2 && n > 14) { /* a header that is right up to the tag */
      f[0] = 0xAD;
      f[1] = 5;
      memcpy(f + 2, vec_com_env[0].env + 2, 4);
      f[6] = 1;
      f[7] = f[8] = f[9] = 0;
    }
    CHECK(sensei_commission_channel_recv(&ch, f, n, out, sizeof(out), &ol) < 0);
  }
  CHECK(ch.rx.highest == 0 && ch.rx.bitmap == 0);
  sensei_commission_channel_close(&ch);
  for (int i = 0; i < 500; i++) {
    uint8_t rec[121];
    struct sensei_commission_record r;

    for (size_t j = 0; j < sizeof(rec); j++) {
      seed = seed * 1664525u + 1013904223u;
      rec[j] = (uint8_t)(seed >> 24);
    }
    rec[0] = 1;
    CHECK(sensei_commission_record_decode(C0, rec, sizeof(rec), &r) < 0);
  }
}

int main(void) {
  C0 = sensei_secure_crypto_openssl();
  test_v9();
  test_points();
  test_v10();
  test_v11();
  test_v12();
  test_v13();
  test_v14();
  test_v15a();
  test_x5_channel();
  test_v15b();
  test_v15c();
  test_v15d();
  test_flow();
  test_aborts();
  test_tamper();
  test_time();
  test_counting();
  test_backend_faults();
  test_garbage();
  return CHECK_DONE();
}
