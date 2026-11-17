/*
 * ----------------------------------------------------------------------
 *
 * File: bench.c
 *
 * Copyright (C) 2025 ETH Zurich and University of Bologna
 *
 * Based on the semester project of Ahmed Malanovic (ETH Zurich, 2025).
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Micro benchmarks of the SENSEI crypto library.
 * Reports the mean time per operation in cycles and microseconds.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#include "crypto/sensei_crypto.h"
#include "example.h"

#define BENCH_BUF_SIZE 4096
#define RUNS_FAST 50 /* hashing, AEAD, HKDF */
#define RUNS_SLOW 5  /* elliptic curve operations */

static uint8_t buf[BENCH_BUF_SIZE];
static uint8_t out[BENCH_BUF_SIZE + SENSEI_CRYPTO_AES_GCM_TAG_SIZE];
static uint8_t dec[BENCH_BUF_SIZE];

static void report(const char *name, size_t size, uint64_t total_cycles, int runs, bool ok) {
  if (!ok) {
    printk("%-28s %5u B  FAILED\n", name, (unsigned int)size);
    return;
  }
  uint64_t cycles = total_cycles / (uint64_t)runs;
  uint64_t us = timing_cycles_to_ns(cycles) / 1000U;

  printk("%-28s %5u B  %10llu cycles  %8llu us\n", name, (unsigned int)size, (unsigned long long)cycles,
         (unsigned long long)us);
}

/* Time a code block `runs` times. `stmt` must set `ok` to false on failure. */
#define BENCH(name, size, runs, stmt)                                                                                  \
  do {                                                                                                                 \
    bool ok = true;                                                                                                    \
    timing_t t0 = timing_counter_get();                                                                                \
    for (int r = 0; r < (runs); r++) {                                                                                 \
      stmt;                                                                                                            \
    }                                                                                                                  \
    timing_t t1 = timing_counter_get();                                                                                \
    report(name, size, timing_cycles_get(&t0, &t1), runs, ok);                                                         \
  } while (0)

void run_benchmarks(void) {
  static const size_t sizes[] = {16, 256, 1024, 4096};
  sensei_crypto_key_id_t key = 0;
  uint8_t nonce[SENSEI_CRYPTO_AES_GCM_NONCE_SIZE] = {0};
  uint8_t digest[SENSEI_CRYPTO_SHA256_SIZE];
  uint8_t okm[42];
  size_t olen = 0;

  memset(buf, 0xa5, sizeof(buf));
  timing_init();
  timing_start();

  printk("\n=== SENSEI crypto benchmarks (CPU %u MHz) ===\n", timing_freq_get_mhz());

  for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
    BENCH("SHA-256", sizes[i], RUNS_FAST, ok &= sensei_crypto_sha256(buf, sizes[i], digest) == SENSEI_CRYPTO_OK);
  }

  for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
    BENCH("HMAC-SHA256", sizes[i], RUNS_FAST,
          ok &= sensei_crypto_hmac_sha256(buf, 32, buf, sizes[i], digest) == SENSEI_CRYPTO_OK);
  }

  BENCH("HKDF-SHA256 (42 B output)", 32, RUNS_FAST,
        ok &= sensei_crypto_hkdf_sha256(buf, 32, buf, 16, buf, 16, okm, sizeof(okm)) == SENSEI_CRYPTO_OK);

  if (sensei_crypto_aead_generate_key(256, &key) == SENSEI_CRYPTO_OK) {
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
      BENCH("AES-256-GCM encrypt", sizes[i], RUNS_FAST,
            ok &= sensei_crypto_aead_encrypt(key, nonce, NULL, 0, buf, sizes[i], out, sizeof(out), &olen) ==
                  SENSEI_CRYPTO_OK);
      sensei_crypto_aead_encrypt(key, nonce, NULL, 0, buf, sizes[i], out, sizeof(out), &olen);
      size_t clen = olen;
      BENCH("AES-256-GCM decrypt", sizes[i], RUNS_FAST,
            ok &=
            sensei_crypto_aead_decrypt(key, nonce, NULL, 0, out, clen, dec, sizeof(dec), &olen) == SENSEI_CRYPTO_OK);
    }
    sensei_crypto_destroy_key(key);
  }

  {
    sensei_crypto_key_id_t a = 0, b = 0;
    uint8_t bpub[SENSEI_CRYPTO_P256_PUBLIC_KEY_SIZE];
    uint8_t secret[SENSEI_CRYPTO_ECDH_SECRET_SIZE];
    size_t blen = 0;

    BENCH("ECDH P-256 generate key", 0, RUNS_SLOW, {
      ok &= sensei_crypto_ecdh_generate_keypair(&a) == SENSEI_CRYPTO_OK;
      if (ok) {
        sensei_crypto_destroy_key(a);
      }
    });

    if (sensei_crypto_ecdh_generate_keypair(&a) == SENSEI_CRYPTO_OK &&
        sensei_crypto_ecdh_generate_keypair(&b) == SENSEI_CRYPTO_OK &&
        sensei_crypto_ecdh_export_public_key(b, bpub, sizeof(bpub), &blen) == SENSEI_CRYPTO_OK) {
      BENCH("ECDH P-256 shared secret", 0, RUNS_SLOW,
            ok &= sensei_crypto_ecdh_shared_secret(a, bpub, blen, secret, sizeof(secret), &olen) == SENSEI_CRYPTO_OK);
      sensei_crypto_destroy_key(a);
      sensei_crypto_destroy_key(b);
    }
  }

  {
    sensei_crypto_key_id_t k = 0;
    uint8_t sig[SENSEI_CRYPTO_ECDSA_SIGNATURE_SIZE];

    BENCH("ECDSA P-256 generate key", 0, RUNS_SLOW, {
      ok &= sensei_crypto_ecdsa_generate_keypair(&k) == SENSEI_CRYPTO_OK;
      if (ok) {
        sensei_crypto_destroy_key(k);
      }
    });

    if (sensei_crypto_ecdsa_generate_keypair(&k) == SENSEI_CRYPTO_OK) {
      BENCH("ECDSA P-256 sign (hash)", 32, RUNS_SLOW,
            ok &=
            sensei_crypto_ecdsa_sign_hash(k, digest, sizeof(digest), sig, sizeof(sig), &olen) == SENSEI_CRYPTO_OK);
      BENCH("ECDSA P-256 verify (hash)", 32, RUNS_SLOW,
            ok &= sensei_crypto_ecdsa_verify_hash(k, digest, sizeof(digest), sig, sizeof(sig)) == SENSEI_CRYPTO_OK);
      sensei_crypto_destroy_key(k);
    }
  }

  timing_stop();
  printk("=== benchmarks done ===\n");
}
