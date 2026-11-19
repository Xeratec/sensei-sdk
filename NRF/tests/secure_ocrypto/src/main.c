/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/* Build check of the commissioning on the nRF5340 (CONFIG_SENSEI_SECURE_SPAKE2P_OCRYPTO), and a self-check for the
 * first run on a board: the ocrypto point arithmetic against the RFC 9383 vectors (V9) and a verifier session against
 * a recorded session (V11, first registration). Untested on hardware; it prints PASS or FAIL on the console. Without
 * CONFIG_SENSEI_SECURE_COMMISSION this is only the key store of the node (the size to compare with).
 * NODE_FOOTPRINT (EXTRA_CFLAGS): no self-check and no vectors, only the calls that a node makes (verifier, record,
 * settings, fingerprint and proof), so that the size of the image minus the size of the build without the commissioning
 * is what a node pays. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "sensei_e2e.h"
#include "sensei_e2e_settings.h"

#ifdef CONFIG_SENSEI_SECURE_COMMISSION
#include "sensei_commission.h"
#include "sensei_commission_settings.h"
#ifndef NODE_FOOTPRINT
#include "vectors_commission.h"
#endif

#ifdef NODE_FOOTPRINT
/* What the node's commissioning code calls (the state machine around it is in the node repository). */
static int node_calls(void) {
  struct sensei_commission_verifier v;
  struct sensei_commission_verifier_cfg cfg = {.c = sensei_secure_crypto_zephyr(),
                                               .save_counters = sensei_commission_settings_counters_save};
  struct sensei_commission_record rec;
  struct sensei_commission_counters cnt;
  uint8_t b[400], c[65];
  size_t n;
  volatile int k = 0;

  k += sensei_commission_settings_verifier_load(&rec) + sensei_commission_settings_verifier_store(&rec);
  k += sensei_commission_settings_counters_load(&cnt) + sensei_commission_settings_verifier_erase();
  k += sensei_commission_settings_counters_erase();
  k += sensei_commission_record_decode(cfg.c, b, 121, &rec) + sensei_commission_record_encode(cfg.c, &rec, b);
  k += sensei_commission_verifier_begin(&v, &cfg, b, 0, b) + sensei_commission_verifier_pake1(&v, 0, b, c, b);
  k += sensei_commission_verifier_pake3(&v, 0, b) + sensei_commission_verifier_seal(&v, 0, b, 4, b, sizeof(b));
  k += sensei_commission_verifier_recv(&v, 0, b, 4, b, sizeof(b), &n) + sensei_commission_verifier_poll(&v, 0);
  k += (int)sensei_commission_verifier_sid(&v) + (int)sensei_commission_verifier_state(&v);
  sensei_commission_verifier_end(&v);
  k += sensei_commission_fingerprint(cfg.c, 1, b, b) + sensei_commission_collector_fingerprint(cfg.c, b, b);
  sensei_commission_proof_plain(1, b, b, b);
  return k;
}
#else

static uint8_t rng_buf[128];
static size_t rng_len, rng_pos;

static int det_random(void *ctx, uint8_t *out, size_t len) {
  ARG_UNUSED(ctx);
  if (rng_pos + len > rng_len) {
    return -1;
  }
  memcpy(out, rng_buf + rng_pos, len);
  rng_pos += len;
  return 0;
}

static void rng_load(const uint8_t *nonce_n, const uint8_t *scalar) {
  rng_len = rng_pos = 0;
  if (nonce_n) {
    memcpy(rng_buf, nonce_n, 16);
    rng_len = 16;
  }
  memset(rng_buf + rng_len, 0, 8);
  memcpy(rng_buf + rng_len + 8, scalar, 32);
  rng_len += 40;
}

static int save_ram(void *user, const struct sensei_commission_counters *cnt) {
  *(struct sensei_commission_counters *)user = *cnt;
  return 0;
}

static int selftest(void) {
  const struct sensei_secure_crypto *c = sensei_secure_crypto_zephyr();
  struct sensei_secure_crypto ct = *c;
  const vec_com_ses_t *s = &vec_com_ses[0];
  struct sensei_commission_record rec;
  struct sensei_commission_counters cnt = {0, 0}, flash = {0, 0};
  struct sensei_commission_verifier_cfg cfg = {0};
  struct sensei_commission_verifier v;
  uint8_t a[65], b[65], z[65], vv[65], red[32], nn[16], sv[65], cv[32], out[40];
  size_t ol = 0;
  int fails = 0;

#define EXPECT(cond)                                                                                                   \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printk("FAIL line %d: %s\n", __LINE__, #cond);                                                                   \
      fails++;                                                                                                         \
    }                                                                                                                  \
  } while (0)

  for (int i = 0; i < VEC_COM_RED_COUNT; i++) {
    EXPECT(c->scalar_reduce(c->ctx, vec_com_red[i].in, red) == 0 && memcmp(red, vec_com_red[i].out, 32) == 0);
  }
  for (int i = 0; i < VEC_COM_BAD_COUNT; i++) {
    if (vec_com_bad[i].len == 65) {
      EXPECT(c->spake2p_check(c->ctx, vec_com_bad[i].data) != 0);
    }
  }
  EXPECT(c->spake2p_check(c->ctx, vec_com_m) == 0);
  EXPECT(c->spake2p_share(c->ctx, vec_com_rfc_w0, vec_com_rfc_x, vec_com_m, a) == 0 &&
         memcmp(a, vec_com_rfc_share_p, 65) == 0);
  EXPECT(c->spake2p_share(c->ctx, vec_com_rfc_w0, vec_com_rfc_y, vec_com_n, b) == 0 &&
         memcmp(b, vec_com_rfc_share_v, 65) == 0);
  EXPECT(c->spake2p_zv(c->ctx, vec_com_rfc_w0, NULL, vec_com_rfc_y, vec_com_rfc_share_p, vec_com_m, vec_com_rfc_l, z,
                       vv) == 0 &&
         memcmp(z, vec_com_rfc_z, 65) == 0 && memcmp(vv, vec_com_rfc_v, 65) == 0);
  EXPECT(c->spake2p_zv(c->ctx, vec_com_rfc_w0, vec_com_rfc_w1, vec_com_rfc_x, vec_com_rfc_share_v, vec_com_n, NULL, z,
                       vv) == 0 &&
         memcmp(z, vec_com_rfc_z, 65) == 0 && memcmp(vv, vec_com_rfc_v, 65) == 0);

  /* the node: session 0 of V11 with the recorded random numbers */
  ct.random = det_random;
  EXPECT(sensei_commission_record_decode(c, vec_com_reg[0].record, SENSEI_COMMISSION_RECORD_SIZE, &rec) == 0);
  cfg.c = &ct;
  cfg.record = &rec;
  cfg.node_id = s->node_id;
  cfg.counters = &cnt;
  cfg.save_counters = save_ram;
  cfg.user = &flash;
  rng_load(s->nonce_n, s->y);
  EXPECT(sensei_commission_verifier_begin(&v, &cfg, s->nonce_g, 0, nn) == 0 && memcmp(nn, s->nonce_n, 16) == 0);
  EXPECT(sensei_commission_verifier_pake1(&v, 1, s->share_p, sv, cv) == 0 && memcmp(sv, s->share_v, 65) == 0 &&
         memcmp(cv, s->confirm_v, 32) == 0 && flash.fails_since_ok == 1);
  EXPECT(sensei_commission_verifier_pake3(&v, 2, s->confirm_p) == 0 && flash.fails_since_ok == 0);
  EXPECT(sensei_commission_verifier_sid(&v) == s->sid);
  EXPECT(sensei_commission_verifier_recv(&v, 3, vec_com_env[0].env, vec_com_env[0].env_len, out, sizeof(out), &ol) ==
             0 &&
         ol == vec_com_env[0].plain_len && memcmp(out, vec_com_env[0].plain, ol) == 0);
  sensei_commission_verifier_end(&v);

  {
    struct sensei_commission_prover p;
    uint8_t sp[65], cp[32];

    rng_load(NULL, s->x);
    EXPECT(sensei_commission_prover_begin(&p, &ct, vec_com_reg[0].w0, vec_com_reg[0].w1, s->node_id, &rec.params,
                                          s->nonce_g, s->nonce_n) == 0);
    EXPECT(sensei_commission_prover_share(&p, sp) == 0 && memcmp(sp, s->share_p, 65) == 0);
    EXPECT(sensei_commission_prover_finish(&p, s->share_v, s->confirm_v, cp) == 0 && memcmp(cp, s->confirm_p, 32) == 0);
    sensei_commission_prover_end(&p);
  }
  /* the records of the key store of the commissioning (settings) */
  (void)sensei_commission_settings_counters_load(&cnt);
  (void)sensei_commission_settings_verifier_load(&rec);
  return fails;
}
#endif /* NODE_FOOTPRINT */
#endif /* CONFIG_SENSEI_SECURE_COMMISSION */

static struct sensei_e2e e2e;

int main(void) {
  struct sensei_e2e_nv nv;
  uint8_t pt[16] = {0}, env[64];

  (void)settings_subsys_init();
  (void)sensei_e2e_settings_load(&nv);
  if (sensei_e2e_init(&e2e, sensei_secure_crypto_zephyr(), &sensei_e2e_settings_ops, NULL, &nv, 16) == 0) {
    (void)sensei_e2e_seal(&e2e, SENSEI_SECURE_KIND_UPLINK, pt, sizeof(pt), env, sizeof(env));
  }
#ifdef CONFIG_SENSEI_SECURE_COMMISSION
#ifdef NODE_FOOTPRINT
  (void)node_calls();
#else
  printk("ocrypto self-check: %s\n", selftest() == 0 ? "PASS" : "FAIL");
#endif
#endif
  return 0;
}
