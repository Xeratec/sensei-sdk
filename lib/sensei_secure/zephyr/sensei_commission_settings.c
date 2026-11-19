/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

#include "sensei_commission_settings.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "sensei_e2e_settings.h"

LOG_MODULE_REGISTER(sensei_commission_settings, CONFIG_LOG_DEFAULT_LEVEL);

#define PFX CONFIG_SENSEI_SECURE_SETTINGS_PREFIX
#define NAME_CMV "e2e_cmv"
#define NAME_CMF "e2e_cmf"
#define CNT_SIZE 8u

#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
#define CMV_STORED_SIZE (SENSEI_COMMISSION_RECORD_SIZE + SENSEI_E2E_SEAL_OVERHEAD)
#else
#define CMV_STORED_SIZE SENSEI_COMMISSION_RECORD_SIZE
#endif

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;

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

/* What a direct load of one record gives. A record of another size than the buffer is noted but not read. */
struct got {
  uint8_t *buf;
  size_t cap;
  size_t len; /* size of the record as stored */
  bool found;
  int err;
};

static int read_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
  struct got *g = param;

  ARG_UNUSED(key);
  g->found = true;
  g->len = len;
  if (len == g->cap && read_cb(cb_arg, g->buf, len) != (ssize_t)len) {
    g->err = -EIO;
  }
  return 0;
}

static int read_record(const char *name, uint8_t *buf, size_t cap, struct got *g) {
  int err;

  memset(g, 0, sizeof(*g));
  g->buf = buf;
  g->cap = cap;
  err = settings_load_subtree_direct(name, read_cb, g);
  return err ? err : g->err;
}

int sensei_commission_settings_verifier_load(struct sensei_commission_record *rec) {
  uint8_t stored[CMV_STORED_SIZE];
  uint8_t plain[SENSEI_COMMISSION_RECORD_SIZE];
  struct got g;
  int err = read_record(PFX "/" NAME_CMV, stored, sizeof(stored), &g);

  if (err == 0 && !g.found) {
    return -ENOENT;
  }
  if (err == 0 && g.len != sizeof(stored)) {
    err = -EBADMSG;
  }
  if (err == 0) {
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
    err = sensei_e2e_settings_unseal(NAME_CMV, stored, sizeof(stored), plain, sizeof(plain));
    if (err >= 0 && err != (int)sizeof(plain)) {
      err = -EBADMSG;
    }
    err = err < 0 ? err : 0;
#else
    memcpy(plain, stored, sizeof(plain));
#endif
  }
  if (err == 0 && sensei_commission_record_decode(sensei_secure_crypto_zephyr(), plain, sizeof(plain), rec) != 0) {
    err = -EBADMSG;
  }
  wipe(plain, sizeof(plain));
  wipe(stored, sizeof(stored));
  return err;
}

int sensei_commission_settings_verifier_store(const struct sensei_commission_record *rec) {
  uint8_t plain[SENSEI_COMMISSION_RECORD_SIZE];
  uint8_t out[CMV_STORED_SIZE];
  int err;

  if (sensei_commission_record_encode(sensei_secure_crypto_zephyr(), rec, plain) != 0) {
    return -EINVAL;
  }
#ifdef CONFIG_SENSEI_SECURE_SETTINGS_SEAL
  err = sensei_e2e_settings_seal(NAME_CMV, plain, sizeof(plain), out, sizeof(out));
  if (err >= 0 && err != (int)sizeof(out)) {
    err = -EIO;
  }
  err = err < 0 ? err : 0;
#elif defined(CONFIG_SENSEI_SECURE_COMMISSION_PLAIN_VERIFIER)
  LOG_WRN("verifier record stored unsealed (no sealing in this build: simulator only)");
  memcpy(out, plain, sizeof(out));
  err = 0;
#else
  err = -ENOTSUP;
#endif
  if (err == 0) {
    err = settings_save_one(PFX "/" NAME_CMV, out, sizeof(out));
  }
  wipe(plain, sizeof(plain));
  wipe(out, sizeof(out));
  return err;
}

int sensei_commission_settings_verifier_erase(void) { return settings_delete(PFX "/" NAME_CMV); }

int sensei_commission_settings_counters_load(struct sensei_commission_counters *cnt) {
  uint8_t b[CNT_SIZE];
  struct got g;
  int err = read_record(PFX "/" NAME_CMF, b, sizeof(b), &g);

  cnt->fails_total = cnt->fails_since_ok = 0xFFFFFFFFu; /* locked unless a good record is read */
  if (err) {
    return err;
  }
  if (!g.found) {
    cnt->fails_total = cnt->fails_since_ok = 0;
    return 0;
  }
  if (g.len != CNT_SIZE) {
    return -EBADMSG;
  }
  cnt->fails_total = get32(b);
  cnt->fails_since_ok = get32(b + 4);
  return 0;
}

int sensei_commission_settings_counters_save(void *user, const struct sensei_commission_counters *cnt) {
  uint8_t b[CNT_SIZE];

  ARG_UNUSED(user);
  put32(b, cnt->fails_total);
  put32(b + 4, cnt->fails_since_ok);
  return settings_save_one(PFX "/" NAME_CMF, b, sizeof(b));
}

int sensei_commission_settings_counters_erase(void) { return settings_delete(PFX "/" NAME_CMF); }
